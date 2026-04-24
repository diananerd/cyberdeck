# Deck 3.0 — UI Bridge

**Status:** Draft. Fifth pillar of the Deck 3.0 spec alongside `DECK-3.0-DRAFT.md` (language), `DECK-3.0-SERVICES.md` (OS foundation + service catalog), `DECK-3.0-CAPABILITIES.md` (consumer protocol), and `DECK-3.0-BUILTINS.md` (in-VM modules). Not yet authoritative. Supersedes `10-deck-bridge-ui.md` once promoted.

**Edition:** 2027.

The UI bridge is the platform component that converts Deck's **semantic content declarations** into concrete presentation — pixels on a display, audio on a speaker, lines in a terminal, glyphs on an e-ink panel — for a specific device. The bridge is the single place where the "how" lives. Apps declare the "what"; the bridge does everything else.

This document defines:

- **Part I** — the meta-spec: what a bridge IS, what it is NOT, and the seven dimensions every bridge declares.
- **Part II** — the content pipeline: how a `content =` block travels from runtime to presentation and how intents travel back.
- **Part III** — the inference rules: how semantic primitives become presentation decisions. Grounded in Gestalt and in substrate-specific rubrics (four axes for spatial substrates; four analogues for voice).
- **Part IV** — UI services: the modal and ambient subsystems the bridge owns (toast, confirm dialog, loading, progress, keyboard, choice overlay, date picker, share sheet, permission dialog, lockscreen, statusbar, navbar, badge).
- **Part V** — subsystems: rotation, theme, brightness, gesture processing, input routing, fonts, assets.
- **Part VI** — system-service integration: which `system.*` services the bridge hosts as their UI backend, and the contract for each.
- **Part VII** — the CyberDeck reference bridge: concrete decisions for the Waveshare ESP32-S3-Touch-LCD-4.3 on LVGL 8.4.
- **Part VIII** — conformance: DL1 / DL2 / DL3 bridge profiles.
- **Part IX** — authoring alternative bridges: the substrate matrix (touch, voice, terminal, e-ink) and the compliance contract.
- **Part X** — changes from Deck 2.0 / `10-deck-bridge-ui.md`.
- **Part XI** — open questions.

---

# Part I — Meta-spec: the shape of a bridge

## 0 · Philosophy

Ten invariants govern every bridge. The first six are language-wide; the last four are bridge-specific.

1. **Apps declare the what; bridges decide the how.** A Deck app writes `content =` and intents. The bridge writes `lv_obj`, `aplay`, `ncurses_addstr`, or whatever its substrate needs. Apps never reference any of that.

2. **One content tree, many presentations.** The same `.deck` file runs on an LVGL touchscreen, an e-ink tablet, a voice assistant, or a terminal. Each bridge renders the same semantic tree differently. The app does not learn which it is running on.

3. **The bridge is not a capability.** Apps do not `@use` the bridge. There is no `bridge.ui.*` capability, no `@grants.ui`, no app-callable method to produce output. The bridge is a **platform component** that listens to the runtime and speaks to system services.

4. **Presentation policy is bridge policy, not language policy.** Spacing values, color palettes, typography scales, gesture thresholds — none of this is in the language spec. The bridge chooses. Apps declare semantic intent; the bridge's policy table decides how that intent surfaces.

5. **Inference beats configuration.** A bridge infers layout, affordance choice, sizing, prominence, and activation patterns from the semantic tree plus device context. It does not take configuration knobs from apps. If a decision needs to vary across apps, the knob lives in `system.theme` (one-shot policy override) or in a new content primitive (semantic extension).

6. **Unix philosophy applied to presentation.** The bridge owns presentation and nothing else. Business logic lives in `@machine`. Persistence lives in `storage.*`. Network lives in `network.*` / `api.*`. Lifecycle lives in `system.apps`. The bridge composes these; it does not duplicate them.

7. **Minimum over maximum.** A bridge ships with the smallest coherent UI vocabulary that satisfies the semantic contract. New overlays, new affordances, and new gestures require justification against existing ones. If a pattern already covers the case, the new one is wrong.

8. **Gestalt is the design frame.** The bridge's inference rules are grounded in five Gestalt principles — proximity, similarity, continuity, closure, common region — measured on four axes: size, position, space, alignment. These are the only policy dimensions a bridge author may tune. Color, typography, and animation are *expressions* of those axes, not independent knobs.

9. **Intent round-trip is lossless.** Every user action the bridge surfaces corresponds to exactly one intent the app declared, carrying exactly the value the app asked for. No affordance exists without a semantic source. No semantic source can be dropped because the bridge decided it "doesn't fit."

10. **The bridge is crash-contained.** A bridge panic does not leak into the runtime or the app's VM. The bridge's failure mode is to fall back to a degraded rendering (plain text, single-column list, one-button dialog; a silent voice turn; a minimal status line) and log `:error` via `system.logs`. Apps continue to run, send intents, and receive transitions even if the bridge is presenting nothing.

## 1 · What a bridge IS

A bridge is a **native platform component** that implements three one-way interfaces:

| Interface | Direction | Spec |
|---|---|---|
| **Content pipeline** | runtime → bridge | DVC snapshots (§6–§7) |
| **Intent pipeline** | bridge → runtime | Intent fire (§8) |
| **UI-service backends** | system services → bridge | Backends for `system.notify`, `system.display`, `system.theme`, `system.security`, `system.share` (§ Part VI) |

Concretely:

- A bridge is a single implementation of the `deck.driver.bridge.ui` SDI vtable (§4).
- Its lifetime begins at platform boot and ends at shutdown.
- It owns its substrate's output surface(s), input pipeline(s), and presentation state — a widget tree on a framebuffer, a turn manager on voice, a character grid on terminal, a ribbon of refreshed regions on e-ink.
- It is the sole renderer on the device; a platform ships exactly one bridge.

## 2 · What a bridge IS NOT

- **Not a Deck capability.** The identifier `bridge.ui` appears only in the SDI catalog, never in an app's `@use` block. No app can import or call the bridge.
- **Not a component library.** Apps do not pick "primary button" vs "ghost button" vs "FAB." They declare `trigger`, `navigate`, `confirm`, `create`; the bridge picks.
- **Not a styling engine.** Apps never specify color, font, spacing, border, radius, opacity, animation, or z-order. The bridge's policy table holds all of these.
- **Not a navigation model.** Apps navigate by `@machine` state transitions. The bridge reflects the current state's `content =`. Pushing / popping activities is an emergent behaviour of state change — not an explicit bridge API.
- **Not a business-logic venue.** The bridge does not cache data, throttle events, merge notifications, or debounce input on behalf of the app. Those concerns live in the app's `@machine` and in the `stream.*` builtin. The bridge's job ends at presentation and intents.

## 3 · The seven dimensions of a bridge implementation

Every bridge is fully specified by seven dimensions. Nothing else belongs to a bridge's declaration.

| # | Dimension | Description |
|---|---|---|
| 1 | **Identity** | A dotted driver ID (`deck.driver.bridge.ui.lvgl`, `deck.driver.bridge.ui.voice`, `deck.driver.bridge.ui.terminal`). Registered under the SDI slot `DECK_SDI_DRIVER_BRIDGE_UI`. |
| 2 | **Substrate** | The presentation medium: `:framebuffer` (pixel display), `:eink` (partial-refresh bitmap), `:voice` (speech I/O), `:terminal` (character grid), `:headless` (pure logic, no output). |
| 3 | **Content coverage** | Which content kinds from §15 of `DECK-3.0-DRAFT.md` the bridge can decode. Gaps degrade to a safe fallback (plain text, dropped render, "not supported" marker) and log `:warn` per first-occurrence-per-view. |
| 4 | **UI-service backends** | Which `system.*` services host their UI on this bridge. Typically `system.notify` (toast), `system.display` (brightness/rotation), `system.theme` (palette swap), `system.security` (lockscreen), `system.share` (share sheet). A headless or voice bridge implements a subset; a terminal bridge renders toasts as status-line text. |
| 5 | **Input substrate** | How user input arrives: `:touch` / `:keyboard` / `:remote` / `:voice` / `:none`. Each substrate maps to a distinct inference policy for intents (e.g., a voice bridge disambiguates `choice` via prompt + readback, not an overlay list). |
| 6 | **Conformance profile** | `:DL1` / `:DL2` / `:DL3`. Fixes the minimum content / UI-service coverage per §58. |
| 7 | **Inference policy pack** | The finite catalog of decisions the bridge makes for "if semantic X, then presentation Y." Part III defines a reference policy pack; a bridge may deviate from any reference rule provided the resulting behaviour still satisfies Part VIII's invariants. |

A bridge may declare capability flags for the SDI registry:

```c
#define DECK_BRIDGE_FLAG_ROTATION       (1u << 0)
#define DECK_BRIDGE_FLAG_THEME_SWITCH   (1u << 1)
#define DECK_BRIDGE_FLAG_TOUCH_GESTURES (1u << 2)
#define DECK_BRIDGE_FLAG_VOICE_OUTPUT   (1u << 3)
#define DECK_BRIDGE_FLAG_BADGE          (1u << 4)
#define DECK_BRIDGE_FLAG_MARKDOWN       (1u << 5)
#define DECK_BRIDGE_FLAG_MEDIA_IMAGE    (1u << 6)
```

Flags are read-only advertisements; they do not gate content decoding.

## 4 · Driver contract (SDI)

The bridge is registered via the `deck.driver.bridge.ui` SDI vtable. The v3 contract refines `12-deck-service-drivers §6.3`. The vtable is stratified into **Core** (every bridge implements), **Visual** (bridges with a raster or character output surface), **Visual-input** (bridges with on-screen text entry), and **Physical-display** (bridges with a pixel-controllable panel). A voice-only bridge implements Core plus whichever Visual methods map sensibly onto voice (often none); a terminal bridge implements Core + Visual; the CyberDeck reference bridge implements all four layers.

```c
typedef struct {
  DeckServiceHeader  header;
  DeckLifecycleOps   lc;

  /* === Core (every bridge implements) ==================================== */

  /* Content pipeline (§6) */
  DeckResult (*push_snapshot)(void *h, const DeckContentSnapshot *snap);
  DeckResult (*clear)        (void *h, uint32_t app_id);

  /* Intent pipeline (§8) */
  void       (*set_intent_hook)(void *h, DeckIntentFn fn, void *user);

  /* Resolution services — every bridge surfaces these in its substrate
     (voice reads a prompt; touch shows an overlay; terminal opens a readline).
     The bridge is free to represent the service non-visually.  */
  void       (*toast)           (void *h, const DeckToastSpec *s);
  void       (*confirm)         (void *h, const DeckConfirmSpec *s);
  void       (*loading_show)    (void *h, const char *label);
  void       (*loading_hide)    (void *h);
  void       (*progress_show)   (void *h, const DeckProgressSpec *s);
  void       (*progress_set)    (void *h, float pct);        /* -1 = indeterminate */
  void       (*progress_hide)   (void *h);
  void       (*choice_show)     (void *h, const DeckChoiceSpec *s);
  void       (*multiselect_show)(void *h, const DeckMultiselectSpec *s);
  void       (*date_show)       (void *h, const DeckDateSpec *s);
  void       (*share_show)      (void *h, const DeckShareSpec *s);
  void       (*permission_show) (void *h, const DeckPermissionSpec *s);

  /* Security — every bridge can enter / exit a locked mode that blocks
     content from being visible / audible until `sec.verify_pin` succeeds.
     Voice bridges answer only "say your PIN" while locked.  */
  DeckResult (*set_locked)      (void *h, bool locked);

  /* Theme — universal as an atom passthrough. Visual bridges swap palette;
     voice bridges may adjust prosody profile; terminal bridges may remap
     ANSI colour set. Bridges that do nothing with the atom are conformant. */
  DeckResult (*set_theme)       (void *h, DeckThemeId theme);

  /* === Visual (bridges with a raster or character output surface) ======== */

  /* Inline text entry. Voice substrates do not implement — they ask. */
  void       (*keyboard_show)   (void *h, DeckKeyboardKind k);
  void       (*keyboard_hide)   (void *h);

  /* Persistent indicators. Only visual substrates dedicate screen real
     estate to statusbar + navbar. Voice and headless bridges stub. */
  void       (*set_statusbar)   (void *h, const DeckStatusbarSpec *s);
  void       (*set_navbar)      (void *h, const DeckNavbarSpec *s);
  void       (*set_badge)       (void *h, const char *app_id, int count);

  /* === Physical-display (bridges with a pixel-controllable panel) ======== */

  DeckResult (*set_rotation)    (void *h, DeckRotation rot);   /* screens only */
  DeckResult (*set_brightness)  (void *h, float level);        /* screens only */
} DeckBridgeUiDriver;
```

**Stubbing convention.** A bridge that does not implement a method for its substrate returns `DECK_SDI_ERR_NOT_SUPPORTED`; the caller (typically a system service) treats that as the device not having the feature. Example: `system.display.set_rotation` on a voice-only platform returns `:err :unavailable` because the bridge's `set_rotation` is stubbed.

Each spec struct is a flat record with primitive-typed fields (no Deck-level heap pointers; all strings are NUL-terminated byte-ranges owned by the caller until the vtable method returns). Full shapes are in §25–§35.

## 5 · Runtime envelope

### 5.1 Threading

The bridge runs on its **own platform thread**, not on the runtime thread. Per `DECK-3.0-SERVICES.md §10.1`, every service has one logical scheduler thread; the bridge follows the same rule.

Cross-thread contract:

| Direction | Serialisation |
|---|---|
| Runtime → bridge (content snapshots) | Lock-free MPSC queue; bridge drains on its frame cadence. |
| Bridge → runtime (intent fire) | Runtime's event queue (§14.10 of DRAFT); intents arrive as cooperative scheduler events. |
| System service → bridge (toast, set_theme, …) | Direct vtable call; vtable implementations schedule the work onto the bridge thread internally. |

A bridge's vtable methods **may be called from any thread**. The bridge is responsible for funnelling them onto its own scheduler. Callers do not hold the bridge's lock across a vtable call.

### 5.2 Memory

- The bridge owns its own heap budget, not the app's. Per-device fixed at platform boot.
- Content snapshots are ref-counted arena allocations; the runtime owns the arena, the bridge borrows for the duration of `push_snapshot` and drops the borrow before returning.
- String fields in spec structs are valid only during the vtable call. A bridge that needs a string after the call returns must copy it into its own heap.

### 5.3 Performance envelope

Two metrics are **universal** (every bridge, every substrate):

| Metric | Target | Failure mode |
|---|---|---|
| Content decode (`push_snapshot` → bridge-internal tree ready) | ≤ 50 ms on reference hardware | `:warn` log entry |
| Intent fire → runtime ack | ≤ 5 ms | `:warn` log entry |

The remaining envelope is **substrate-specific** — a framebuffer bridge measures frame time; a voice bridge measures time-to-first-syllable; a terminal bridge measures time-to-first-character. These are documented in each bridge's own reference chapter (see §58 for the CyberDeck touchscreen targets).

Bridges are **not required** to hit any particular target. The targets exist so platforms can calibrate their content pipeline; a platform that cannot meet a conformance-level envelope declares a reduced `@needs.deck_level` in its manifest.

### 5.4 Content-tree bounds

A bridge MUST accept snapshots within these structural bounds. Beyond them the bridge MAY truncate with a `:warn` log.

| Bound | Value |
|---|---|
| Max depth | 8 levels |
| Max siblings per parent | 256 |
| Max total nodes per snapshot | 1024 |
| Max string attribute length | 4 KB |
| Max list items in one render | 256 (use `has_more:` for more) |

Apps exceeding bounds see the truncation reflected in the rendered output; a `list` with 512 items shows the first 256 and an implicit "LOAD MORE" affordance appended by the bridge.

---

# Part II — The content pipeline

## 6 · Content tree lifecycle

A single `content =` block flows through five stages:

```
  (Deck source)   content = list posts …
        │
        │  parser → AST (§15 of DRAFT)
        ▼
  Content AST     ContentList { expr: Posts, binder: :p, body: [...] }
        │
        │  evaluator (dependency-tracked against machine state + @config)
        ▼
  Content value   DVC tree — node kinds, attributes, intent_ids
        │
        │  serializer (§18 of 11-deck-implementation.md)
        ▼
  DVC snapshot    byte sequence with app_id, machine_id, state_id, frame_id
        │
        │  bridge.ui.push_snapshot(snap)
        ▼
  Presentation    lv_obj tree, text-to-speech queue, terminal grid, …
```

**Re-evaluation triggers** — the runtime re-evaluates `content =` after any event that touches a dependency of the content body:

- A `@machine` transition committed successfully (new state, new payload).
- `config.set(:key, _) → :ok` for a key the content body reads.
- A named source `N` emitted and the content body reads `N.last()` / `N.recent(_)` / `N.count()`.
- A pure builtin called inside the body changed result for an input it depends on (dep tracking is transitive over pure calls).

Re-evaluation produces a new DVC snapshot and pushes it. The bridge is responsible for diffing (§9).

## 6.1 · Why this cannot loop

A fair question for anyone who has written a reactive UI before: "if state change produces render and user input produces state change, can the render cause state to change?" In Deck the answer is **structurally no**, for four reinforcing reasons.

### 6.1.1 The pipeline is unidirectional

The bridge can do exactly two things: **present** the current snapshot and **fire intents on user activation**. It cannot write `@config`, cannot call `Machine.send`, cannot touch `@machine` payloads. Rendering produces no events. An intent requires either a genuine user action (a tap, an utterance, a keypress) or an emission from a declared external source (a stream, a timer, an OS event) — never the act of rendering itself.

### 6.1.2 Content bodies are pure

Per `DRAFT.md §4.5`, a `content =` body is a pure expression. It may call pure builtins, read `config.<key>`, read `<MachineName>.state`, read `NamedSource.last/recent/count`, and compose content nodes. It **cannot** call capabilities, mutate config, send to machines, emit logs, or invoke `!` fns. The body's evaluation has no observable effect outside the DVC tree it produces.

Any attempt to put an impure call inside `content =` is a `LoadError :type` — caught at load, before the app runs.

### 6.1.3 Re-evaluation is dependency-tracked, not schedule-driven

The runtime does not poll. It does not re-render on a frame cadence. It re-evaluates `content =` *only* after an event that touches a dependency the loader recorded at load time (§6, enumerated above). The dependency set of a body is computed statically; a body whose dependency set is empty is a `LoadError :type` (per `DRAFT.md §14.10`) — a body that cannot change cannot exist.

Critical consequence: **the act of producing a new snapshot does not touch any dependency key**. Rendering the new snapshot cannot, by itself, invalidate the body that produced it. There is no `render → state-read → state-write → render` cycle because the middle arrow is forbidden by §6.1.2.

### 6.1.4 Patched updates do not fire input handlers

When the runtime patches a leaf attribute (a `value:` on a `toggle`, the text in a `data` node, a progress bar's fill), the bridge updates the affordance in place (§9). **This does not fire `on change` / `on complete` / `on submit`** — those handlers only fire on genuine user activation. A `toggle` whose `value:` flipped because the machine transitioned is visually updated, not re-submitted. Without this rule, every patched update would race the machine's outgoing `send` against the affordance's incoming one.

### 6.1.5 The only legitimate cycles are author-chosen

Some apps *want* ongoing activity: a timer UI that updates every second, a stream subscription that re-renders on every emission, a state machine that self-transitions via `@on watch cond -> Machine.send(...)`. These are bounded by four mechanisms:

| Scenario | How the loop is bounded |
|---|---|
| `@on every D -> Machine.send(...)` | Interval `D` ≥ 50 ms (scheduler floor, SERVICES §34); background budget caps total handler time (`DRAFT.md §22.3`, default 500 ms per 30 s). |
| `@on watch cond -> action` | **Edge detection only** — fires on `false → true` transitions, not while `true`. A self-cancelling watch (action makes `cond` false) fires once per external cause. A watch toggling >100 fires/sec is runtime-capped with `:warn` (`DRAFT.md §14.10`). |
| `state :s on enter -> Machine.send(:next)` | One shot per state entry (hooks run exactly once, `DRAFT.md §13.5`). A state whose `on enter` sends to itself is infinite — but it is one send per enter, serialised by the cooperative scheduler, and `max_run_ms` (default 200 ms) kills it with `@on overrun`. |
| Stream pipelines (`@on source <stream> -> ...`) | Emissions come from outside the VM (timer, OS event, network). Rate is inherent to the source; back-pressure is a bounded event queue (default 32, oldest dropped with `:warn`). |

An author who writes a state machine that self-transitions continuously will hit `@on overrun`, quarantine after 3 panics in 5 min, and surface as `:err :service_unavailable`. The runtime contains the bug; the bridge keeps rendering the last-known snapshot.

### 6.1.6 Summary

```
  User action ──────┐
  Stream emission ──┼──► intent / event ──► transition ──► new state
  Timer fire  ──────┤                                            │
  OS event ─────────┘                                            │
                                                                 ▼
                                    dependency-tracked re-evaluation
                                                 │
                                                 ▼
                                        new DVC snapshot
                                                 │
                                                 ▼
                                     bridge diff + render
                                                 │
                                                 ▼
                                       (no further effect)
```

The arrow from render back to state does not exist. That is the load-bearing invariant.

## 7 · DVC snapshot — wire shape

The on-wire format is specified authoritatively in `11-deck-implementation.md §18`. This section restates only the semantics relevant to bridge authors.

Each snapshot carries:

```
magic           0xDC0E
version         1
app_id          uint32 — which app the content belongs to
machine_id      uint32 — which @machine the state belongs to
state_id        uint32 — intern ID of the state atom
frame_id        uint32 — monotonically increasing per (app, machine)
tree_bytes      variable — node tree (DFS preorder)
```

**What the snapshot deliberately does NOT carry.** Presentation context — orientation, pixel density, viewport geometry, input modality, theme, locale formatting preferences, time-of-day — is **bridge-internal state**, not part of the wire format. The runtime does not know whether the device has a screen, what size, or whether it rotates. Including such fields on the wire would be a layering violation: the runtime would have to distinguish substrates, and every alternative bridge (voice, terminal, e-ink) would have to tolerate or forge fields irrelevant to itself. The bridge consults its own substrate state (§ Part V) when rendering; the runtime consults nothing about the substrate ever.

Each node carries:

```
kind            uint8 — §7.1 catalog
flags           uint8 — {:focusable, :keep_visible, :scroll_anchor, ...}
intent_id       uint32 — 0 if the node is not actionable
attr_count      uint8
children_count  uint16
[attrs]         attr_count × (atom_id, type, value)
[children]      children_count × node
```

### 7.1 Content kinds (the bridge's decoder switch)

Kinds are atoms in the Deck language; on the wire they are one-byte codes. The bridge decodes by kind and dispatches to a kind-specific renderer. The catalog matches `DRAFT.md §15` exactly:

**Structural (§15.1):**
- `:list` — scrollable collection; children are either list items (via `binder ->`) or an empty marker.
- `:list_item` — per-item wrapper (emitted implicitly by the runtime around each iteration of `list`'s binder body).
- `:list_empty` — the `on empty ->` body, rendered only when the list expression is `[]`.
- `:group` — semantic group; has `:label` string attribute.
- `:form` — cohesive input group; carries `:submit_label` and an `intent_id` for the submit event.

**State markers (§15.2):**
- `:loading` — no attributes; bridge picks affordance.
- `:error` — has `:reason` string attribute.

**Data wrappers (§15.3):**
- `:data` — bare typed expression rendered by type; has `:value` attribute (int/float/str/Timestamp) and `:type_hint` atom.
- `:media` — has `:src` (str or AssetRef), `:alt`, and optional `:role` (`:avatar`/`:cover`/`:thumbnail`/`:inline`).
- `:rich_text` — has `:value` string with inline `**` / `*` / `` ` `` / `[text](url)` markers.
- `:status` — label+value pair; has `:label` and `:value` attributes.
- `:chart` — has `:values` list and optional labels.
- `:progress` — has `:value` (float 0..1 or −1 for indeterminate) and `:label`.
- `:markdown` — has `:value` str, `:purpose`, optional `:focus`, `:describe`, and `on link` / `on image` intent_ids.
- `:markdown_editor` — has `:value`, `:placeholder`, `:describe`, and intent_ids for change/cursor/selection.

**Input intents (§15.4):**
- `:toggle` / `:range` / `:choice` / `:multiselect` / `:text` / `:password` / `:pin` / `:date` / `:search` — each with `:name` atom, `:value` current, kind-specific bounds (`:min`/`:max`/`:step`/`:length`/`:options`), and the `on change` / `on complete` intent_id.

**Action intents (§15.4):**
- `:trigger` / `:navigate` / `:create` — each with `:label`, optional `:badge`, and the direct `->` intent_id.
- `:confirm` — has `:label`, `:prompt`, and the post-confirmation intent_id.

**Passive intent (§15.4):**
- `:share` — has an optional `:label` and the target-expression payload.

**Control (§15.7):** `when` and `for` are resolved before serialization — they do not appear as DVC nodes. A `when cond` that is `false` emits zero children. A `for x in xs` emits one subtree per `x`.

### 7.2 Attribute types

Per `deck_dvc.h`:

| Type code | Meaning |
|---|---|
| `:bool` | boolean |
| `:i64` | 64-bit signed int |
| `:f64` | 64-bit float |
| `:str` | NUL-terminated UTF-8, length-prefixed |
| `:atom` | atom name as string (bridge looks up intern ID) |
| `:list_str` | list of strings (e.g., `:options` labels) |
| `:asset_ref` | opaque handle resolved via `assets.*` |

### 7.3 Intent ID allocation

Intent IDs are **u32 handles minted by the runtime**, not by the app. The runtime's intent table maps `intent_id → (app_id, machine_name, event_atom, params_map?)`. The bridge treats intent IDs as opaque; firing an intent takes the ID and a value payload.

Intent IDs are **snapshot-scoped**. A new snapshot invalidates every intent ID the previous snapshot exposed. The bridge MUST NOT retain intent IDs across snapshots — activations on stale affordances are discarded on the runtime side (the runtime checks the frame_id on fire).

## 8 · Intent round-trip

The bridge fires an intent by calling the runtime-registered hook:

```c
void deck_runtime_fire_intent(uint32_t intent_id,
                               const DeckIntentVal *vals, uint32_t n_vals);
```

Value kinds:

```c
typedef enum {
  DECK_INTENT_VAL_NONE = 0,
  DECK_INTENT_VAL_BOOL,
  DECK_INTENT_VAL_I64,
  DECK_INTENT_VAL_F64,
  DECK_INTENT_VAL_STR,
  DECK_INTENT_VAL_ATOM,
  DECK_INTENT_VAL_LIST_STR,
  DECK_INTENT_VAL_LIST_ATOM,
} DeckIntentValKind;

typedef struct {
  const char          *key;   /* NULL for scalar events; non-NULL for form submits */
  DeckIntentValKind    kind;
  union { bool b; int64_t i; double f; const char *s;
          const char * const *ss; const char * const *atoms; } v;
  uint32_t             n;     /* list length for list kinds */
} DeckIntentVal;
```

**Scalar events** (`on change` for toggle/range/text/…, `on complete` for pin, `->` for trigger/navigate/confirm/create):
- `n_vals == 1`, `vals[0].key == NULL`, `vals[0].kind` matches the intent's value type.
- Action intents (`trigger`/`navigate`/`create`): `n_vals == 0`, `vals == NULL`.
- `confirm`: fires with `n_vals == 0` on user confirmation; **does not fire** on cancel.

**Form submit** (`on submit`):
- `n_vals == N`, one per input intent in the form subtree.
- Every `vals[i].key` is non-NULL — the intent's `:name` atom as a string.
- Order is stable across snapshots but undefined; the runtime keys by name.

**Lifecycle contract** — between `fire_intent` and the next `push_snapshot`, the app's machine processes the event. Re-evaluation produces a new snapshot; the bridge updates. Typical latency on reference hardware: 5–30 ms for pure state transitions, 50–500 ms when the transition runs `@on enter` that hits a capability.

## 9 · Diffing and minimal rerender

A snapshot carries `(app_id, machine_id, state_id, frame_id)`. The bridge maintains the **last rendered snapshot** per `(app_id, machine_id)` pair. On receiving a new snapshot it decides:

| Comparison | Bridge action |
|---|---|
| Different `(app_id, machine_id)` | Push new activity; tear down the previous activity in this slot only after the new one is presented. |
| Same `(app_id, machine_id)`, different `state_id` | Replace the active content in-place; no push/pop. |
| Same `(app_id, machine_id, state_id)`, same tree shape, only leaf attrs changed | **Patch** — update leaf properties in place (text, value, progress, badge). No teardown. |
| Same `(app_id, machine_id, state_id)`, different tree shape | **Rebuild** — teardown and reconstruct the content subtree. |

**Tree-shape equality** is structural: same kinds, same children counts, in the same order. Attributes may differ. Intent IDs are expected to differ — the bridge MUST update its widget → intent_id map on every patch.

A bridge MAY implement full rebuild everywhere (no patching). Patching is a performance optimisation, not a correctness requirement.

## 10 · Content identity

`(app_id, machine_id, state_id)` is the bridge's **identity key** for everything related to a rendered activity:

- Widget tree ownership.
- Overlay routing ("confirm dialog for *this* state").
- Scroll position restoration on re-enter.
- Keyboard focus state.
- Gesture context.

When the identity triple changes, the bridge treats it as a new activity and discards all of the above. When it stays the same across a patch, the bridge preserves all of the above.

**The activity stack** — the bridge maintains a stack of distinct identity triples. `@machine` transitions reuse the top-of-stack slot (same `(app_id, machine_id)` pair; new `state_id`). App launches push. Back navigation (§14.8 of DRAFT + §24 of this document) pops.

Stack depth is bounded (reference: 4). Overflow evicts the **middle** entry, not the top or the launcher (slot 0 — always reserved for the launcher app; never evicted).

---

# Part III — Inference rules (semantic → presentation)

This is where "declare the what, decide the how" becomes concrete. A bridge's inference policy is a finite table mapping semantic contexts to presentation decisions.

**Layering note.** The rules in this Part are split by scope:

- **Universal rules** (§11, §18, §20) — apply to every bridge, every substrate. A bridge that violates one is non-conformant.
- **Spatial-substrate rules** (§12–§17, §19) — apply to bridges that render in a 2-D arrangement (framebuffer, e-ink, terminal grid). They concern size, position, space, alignment, and the visual composition of widgets.
- **Voice-substrate rules** (§12bis) — apply to bridges that present serially through speech. They concern turn order, prominence-by-repetition, pacing.

A substrate-specific rule is never invoked outside its substrate. A voice bridge does not consult "primary-right-filled" (§17); a touchscreen bridge does not consult "repeat-on-pause" (§12bis). Both obey §11 and §18 and §20.

The rules below are the **reference policy** for a 7-inch touchscreen bridge unless marked otherwise. Alternative substrates (voice, terminal, e-ink, smartwatch) diverge as noted; each divergence is justified against the same invariants.

## 11 · Gestalt as the design frame

The inference policy applies five Gestalt principles. Each principle maps to bridge behaviour that turns semantic structure into perceptual structure.

| Principle | Statement | Bridge behaviour |
|---|---|---|
| **Proximity** | Items near each other are perceived as related. | Children of the same `group` render visually close (small inter-child gap); unrelated groups are separated by a wider section gap. |
| **Similarity** | Items that look the same are perceived as peers. | Items in a `list` share a row template; triggers in an action row share a button template; data wrappers of the same type share a typographic treatment. |
| **Continuity** | Elements on a common axis are perceived as a group. | Form fields align on a common left edge; action rows align on a common baseline; list rows align on a common leading. |
| **Closure** | Bounded regions are perceived as units. | Groups render inside a bordered card; confirm dialogs and permission dialogs have hard edges; toasts are fully enclosed. |
| **Common region** | Elements in the same enclosure belong together. | The primary action and its label occupy the same button; a confirm dialog's title, body, and buttons share one dialog; all overlays live on a single z-layer distinct from content. |

These principles are **non-negotiable** across bridges. A voice bridge applies them too: proximity = adjacent utterances, similarity = consistent prompt framing, continuity = ordered read-back, closure = "press 1 to confirm, 2 to cancel" framing, common region = one turn per modal.

## 12 · The four-axis rubric — spatial substrates

*Substrate scope: `:framebuffer`, `:eink`, `:terminal`. Not consulted by voice bridges.*

Every presentation decision a **spatial** bridge makes reduces to four axes. Color, typography, animation, and style are **derived** from these — they are not independent knobs.

| Axis | What it encodes | How the bridge tunes it |
|---|---|---|
| **Size** | Emphasis / hierarchy | Primary CTA wider than secondary; primary data font larger than secondary; main content fills; decorative elements minimal. |
| **Position** | Relationship to the current context | Primary action bottom-right (LTR) — the "exit" of a left-to-right, top-to-bottom reading flow. Destructive actions flanking non-destructive (user must traverse non-destructive first). Status at the top; navigation at the bottom/side. |
| **Space** | Grouping strength | Small padding = tight group; medium padding = loose group; large padding = unrelated sections. Never a divider between unrelated groups — space does that work. |
| **Alignment** | Shared structure | Left-align text; right-align actions; center-align overlays and icons-with-labels inside cards. |

**Colour, typography, animation — derivations:**

- **Primary** vs **dim** colour tracks emphasis (size-axis encoding as hue). A "primary" widget is also larger or more prominent; the colour amplifies the size decision.
- **Font size** (MD vs SM vs LG vs XL) tracks hierarchy (size-axis). A caption is both smaller and dimmer — two reinforcing cues.
- **Animation** tracks state transitions (space-axis: an item "moves into" or "out of" a group). Overlays fade in; scroll is instant; rotation is a rebuild (no transition).
- **Border / radius** tracks closure (common region). Cards have borders; inline text does not.

A bridge author who wants to change "how CyberDeck looks" changes the reference values in Part VII — not the rubric.

## 12bis · The four-axis rubric — voice substrate

*Substrate scope: `:voice`. Not consulted by spatial bridges.*

Voice bridges face a 1-dimensional presentation axis (time). The four analogous axes are:

| Axis | What it encodes | How the bridge tunes it |
|---|---|---|
| **Duration** | Emphasis / hierarchy | Primary CTA spoken with a slower cadence; secondary actions terser. Critical information takes more time (Size analog). |
| **Order** | Relationship to context | Status first, body next, action prompts last — matching the user's reading flow on a spatial substrate (Position analog). Destructive actions spoken after non-destructive so the user must hear the safe option first. |
| **Pause** | Grouping strength | Short pause (100 ms) between items in the same group; medium (300 ms) between groups; long (800 ms) before a modal prompt. Pause replaces space. |
| **Repetition** | Shared structure | Parallel phrasing across siblings ("Item one: … Item two: …"); prompts repeat verbatim on no-input timeout (Alignment analog). |

Prosody, voice, speech rate, and pauses are **expressions** of these axes, not independent knobs — the same way color/typography express size/position/space/alignment on spatial substrates.

## 13 · Structural primitives

### 13.1 `list`

```
Semantic:
  list expr
    on empty -> ...
    has_more: bool_expr
    on more -> ...
    binder ->
      <children>
```

**Reference decisions:**

- Scrollable column.
- Per-item template derived from the child body of `binder`; every item rendered identically (Similarity).
- Dividers between items are part of the **row template**, not a separate node. Reference: 1 px bottom border on each row, so the last row has no hanging divider.
- `on empty -> …` renders centred, dim (50 % primary opacity), MD typography.
- `has_more: true` with `on more -> …` renders a trailing "LOAD MORE" affordance (a bridge-generated trigger) at the list tail. The affordance is not a content node — the bridge supplies it.
- Lazy-load pagination is not a separate primitive; it emerges from `has_more:` + `on more`.

**Substrate variations:**

- Voice bridge: reads the first N items, says "There are more; say 'more' to continue" — `on more` fires on the voice command.
- E-ink bridge: paginates by screen; `has_more` surfaces as a page-forward affordance.
- Terminal bridge: numbered list; `on more` is a keyboard `n`.

### 13.2 `group`

```
Semantic:
  group "label"
    <children>
```

**Reference decisions:**

- Labelled bordered card.
- Label at the top, dim SM typography. An empty label (`group ""`) omits the label row but keeps the card.
- Nested groups: the inner group does **not** re-border on this bridge (avoids visual noise); it instead adds a left inset (Proximity).
- Groups without labels and without siblings may be flattened (bridge discretion).

**Substrate variations:**

- Voice bridge: announces label then enumerates children. An unlabeled group is silent.
- Terminal bridge: underlines label; no border.

### 13.3 `form`

```
Semantic:
  form
    on submit -> ...
    submit_label: "Save"?
    <children>
```

**Reference decisions:**

- Bordered card (same as `group`) plus a terminal action row.
- Bridge appends a `[CANCEL] [SUBMIT_LABEL]` row at the form's tail, space-above to pin to the bottom if the content is short.
- `CANCEL` fires no intent; it dismisses modal state (clears dirty flags, keeps state values unchanged).
- `submit_label:` defaults to `"SAVE"` if absent.
- Forms may contain sibling action intents (`trigger` / `navigate` / `confirm` / `create`); they render inline and fire their own `->` actions directly — they do NOT submit the form.
- A form with zero input intents is a LoadError (§15.1 of DRAFT); the bridge never sees that case.

**Substrate variations:**

- Voice bridge: asks each input in order; reads back summary before `submit` / `cancel`.
- Terminal bridge: renders fields inline; `submit` is <Return>, `cancel` is <Esc>.

## 14 · State markers

### 14.1 `loading`

```
Semantic:
  loading
```

**Reference decisions:**

- If `loading` is the **only root child** of the content tree: show Loading Overlay Service (§27). Full-screen backdrop.
- If `loading` appears inside a list, group, or form: show inline spinner / pulse in its place; rest of the content keeps rendering.
- The `loading` marker carries no fields; a bridge that wants to explain *what* is loading uses `progress label: "Checking update…"` instead.

**Substrate variations:**

- Voice bridge: emits a single prompt "Working…" then silent until the next snapshot.
- Terminal bridge: single-char spinner cycle in place.

### 14.2 `error reason:`

```
Semantic:
  error reason: "Could not reach the server"
```

**Reference decisions:**

- Centred, MD typography, in the theme's destructive colour (reference: theme-specific accent; all three reference themes use `primary` for error so it stays legible).
- Not modal; the app decides what state to be in, and the bridge reflects.
- If the app wants a modal error, it models it with a `:error (reason: …)` state whose `content =` is a form with a single `trigger` to dismiss — the bridge presents whatever the content says.

**Substrate variations:**

- Voice bridge: reads `"Error: {reason}"` once.
- Terminal bridge: red text band.

## 15 · Data wrappers

Each wrapper maps a semantic data-carrier onto a presentation type. The bridge's job is to present values faithfully; the app's job is to declare what kind of value it is.

### 15.1 Bare typed expressions

```
Semantic:
  p.title                    -- str
  n                          -- int / float
  p.created_at               -- Timestamp
  status_atom                -- atom
```

**Reference decisions:**

- `str` / `atom` → MD, primary colour. ALL-CAPS if the reference theme policy mandates (see Part VII).
- `int` / `float` → MD, primary. Numeric formatting follows `system.locale`.
- `Timestamp` → SM, dim. Formatting: relative ("2 min ago") if within 1 day, absolute ("2026-04-23 14:30") otherwise. Policy is bridge's, not app's.
- `bool` → SM, primary for `true` / dim for `false`. Labels read "YES" / "NO" (locale-adjusted).

### 15.2 `media`

```
Semantic:
  media expr
    alt:  str
    role: :avatar | :cover | :thumbnail | :inline
```

**Reference decisions (per role):**

| Role | Size | Shape | Decoration |
|---|---|---|---|
| `:avatar` | Fixed (reference: 56 px) | Round | 1 px dim border |
| `:cover` | Container width × 40 % viewport height max | Rounded rect (16 px) | Crops on overflow |
| `:thumbnail` | Fixed (reference: 96 px) | Rounded rect (8 px) | No border |
| `:inline` | Natural, clamped to container | Rectangle | No border |
| (omitted) | Same as `:inline` | | |

- Image not found / decode error: alt-text rendered as a `data` node in dim SM.
- Network URLs are **not** supported by the reference bridge. Sources must be bundled or cached (`AssetRef` from `@assets` or a local path from `fs`).

**Substrate variations:**

- Voice bridge: reads the alt text only.
- Terminal bridge: outputs `[IMAGE: alt]` line.

### 15.3 `rich_text`

```
Semantic:
  rich_text "**bold** plain *italic* `code` [link](url)"
```

**Reference decisions:**

- MD typography; primary colour.
- Inline markers: `**bold**` → bold weight; `*italic*` → italic weight (or underline if no italic font variant); `` `code` `` → monospace + dim background; `[text](url)` → underlined.
- Block structures (headings, lists, quotes, code blocks, tables) are **not** recognised by `rich_text`. Use `markdown` for that.
- A link activation is inert on this wrapper — `rich_text` has no `on link` intent. Apps that need interactive links use `markdown` with `on link ->`.

### 15.4 `status`

```
Semantic:
  status expr
    label: "Battery"
```

**Reference decisions:**

- Label: SM, dim, above.
- Value: MD, primary, below. Type-dependent formatting per §15.1.
- The pair shares a column; reference column direction is vertical (label-above-value). On a wide landscape screen, the bridge may pivot multiple adjacent `status` pairs into a horizontal grid (Continuity).

### 15.5 `chart`

```
Semantic:
  chart values
    label:   "Heap usage"
    x_label: "time"
    y_label: "KB"
```

**Reference decisions:**

- Reference bridge renders simple line or bar chart for `[float]` / `[(float, float)]`.
- No axis ticks beyond min/max. No legend beyond `label`.
- If the bridge cannot render a chart (DL1, DL2 minimal implementations), it falls back to a `"[chart]"` placeholder and logs `:warn` first-occurrence-per-view.

### 15.6 `progress`

```
Semantic:
  progress
    value: 0.42        -- 0.0..1.0, or -1 for indeterminate
    label: "Downloading…"
```

**Reference decisions:**

- **Root-level** progress node → Progress Overlay Service (§28).
- **Nested** progress node → inline bar. Height 20 px, 2 px dim border, primary fill. If the value is negative, the bar renders an animated zebra ribbon.
- `label` is SM dim above the bar.
- Percentage text ("42%") is drawn centred on the bar when determinate.
- The bridge does not expose a cancel affordance from the `progress` wrapper — if the operation is cancellable, the app models it with a sibling `trigger "Cancel" -> …`.

### 15.7 `markdown`

```
Semantic:
  markdown content
    purpose:  :reading | :reference | :fragment
    focus:    str?
    describe: str?
    on link  -> …
    on image -> …
```

**Reference decisions (per purpose):**

| Purpose | Body font | Heading scale | Line height | Max measure | ToC |
|---|---|---|---|---|---|
| `:reading` | MD | h1=XL, h2=LG, h3=MD-bold | 1.6× | 640 px | ≥ 3 headings |
| `:reference` | MD | h1=LG, h2=MD-bold, h3=MD | 1.4× | full | ≥ 5 headings, sticky |
| `:fragment` | SM | h1=h2=h3=MD-bold | 1.2× | full | never |

- Code blocks: bordered card, monospace, copy affordance (activation → bridge copies to system clipboard; no intent fires).
- Images: sized per `:role` when specified, else scale-to-width.
- Links: activation → `on link` fires with `{url, text}`; if no handler, link is inert.
- `focus: "heading-id"` — bridge brings the target heading into view reactively.

### 15.8 `markdown_editor`

```
Semantic:
  markdown_editor
    value:          str
    placeholder:    str?
    controlled_by:  MdEditorState?
    describe:       str?
    on change    -> …
    on cursor    -> …    (optional)
    on selection -> …    (optional)
```

**Reference decisions:**

- Textarea with monospace-optional markdown rendering.
- Toolbar is **hidden** on touch bridges; long-press surfaces a Choice Overlay with format actions (`:bold`, `:italic`, `:code`, `:heading`, `:bullet_list`, `:ordered_list`, `:blockquote`, `:code_block`, `:link`).
- Preview toggle is a bridge-owned action at the bottom of the editor, not an app-declared affordance.
- Keyboard: shown on `on_resume` (never on `on_create`).
- When `controlled_by:` is non-nil, the bridge renders whatever the `MdEditorState` carries; apps drive cursor/format externally via capability methods.

## 16 · Intent primitives

*Substrate scope: the **commit timing** and **event payload** columns are universal (every bridge obeys the same commit contract); the **reference presentation** column is a spatial-substrate reference, voice/terminal bridges translate per §12bis and per each bridge's own reference chapter.*

Each intent has a kind-specific inference rule. The shared rule is that **every user action the bridge surfaces comes from exactly one intent and carries exactly the declared payload**.

### 16.1 Input intents

| Intent | Reference presentation (spatial) | Commit timing (universal) | Event payload (universal) |
|---|---|---|---|
| `toggle` | Switch widget, label left, switch right | `on change` fires on state flip | `bool` |
| `range` | Slider, value label above (SM, dim) | `on change` fires on **release** (not during drag) | `int`/`float` per declared type |
| `choice` | Tappable row showing current value + `›` | Tap → Choice Overlay Service (§30) | value of selected option |
| `multiselect` | Tappable row showing count ("3 selected") + `›` | Tap → Choice Overlay Service in multi mode | `[V]` list |
| `text` | Textarea with hint placeholder | Keyboard confirm fires `on change` | `str` |
| `password` | Same as `text` + character masking | Same | `str` |
| `search` | Textarea with search icon prefix | `on change` fires **on every keystroke** (debouncing is the app's job via `stream.debounce`) | `str` |
| `pin` | Custom numpad + dot row | `on complete` fires once `length` digits entered and user taps OK | `str` of digits |
| `date` | Tappable row showing current date + `›` | Tap → Date Picker Service (§31) | `Timestamp` at local-midnight |

**Universal input-intent rules:**

- `name:` atom is the form-submit key and the runtime's dependency key. It is not rendered.
- `value:` reflects current state. The widget displays this value on render. Updates to the value from state changes produce a patched render (§9).
- Input intents are **auto-commit** (the bridge fires on the commit event above). No explicit save button.
- A form wrapping input intents overrides the auto-commit — values accumulate in `event.values` and fire on `on submit`. Input intents inside a form do NOT fire individual `on change` actions on commit; the form fires once on submit.

### 16.2 Action intents

| Intent | Reference presentation (spatial) | Bridge routing (universal) |
|---|---|---|
| `trigger` | Outline or filled button (§17 rules) | Direct; fires on activation |
| `navigate` | Activatable row with leading icon + trailing `›` (or equivalent) | Direct; fires on activation |
| `confirm` | Outline button (same as trigger) | Activation → Confirm Dialog Service (§26); fires only on user confirmation |
| `create` | Activatable row with `+` prefix, OR a FAB in the trailing corner on bridges that support FAB | Direct; fires on activation |

**Universal action-intent rules:**

- `label:` is rendered (reference: ALL CAPS per Part VII).
- `badge: int?` renders a badge circle (§35) on the trigger / navigate / create widget when the int is > 0.
- Action intents never carry a value payload to the runtime — the intent's semantic *is* the action.
- `navigate` does not push a new activity by itself — it fires an intent that the machine translates into a state transition, which produces a new snapshot, which the bridge renders by replacing the active content (§9).

### 16.3 Passive intent

- `share` — renders as a bridge-owned affordance (reference: tappable row with upload icon + label). Tap → Share Sheet Service (§32). The action on the target (copy, send to app, save) fires no app-facing intent unless the user dismisses; the target app receives the shared content through `share.target` service.

## 17 · Action composition

*Substrate scope: spatial substrates. Voice bridges compose actions by order (§12bis) — the last action spoken is the primary; `cancel`/`back`/`discard`/`skip`/`later` labels are always spoken first.*

This section encodes the pre-spec demo's hardened UX patterns for spatial substrates.

### 17.1 Single action at content tail

If the content tree ends with a single `trigger` / `confirm` / `create` as a direct child of the root (or of a form's action slot):

- Rendered full-width, filled primary.
- Pinned to the tail of the content area (implicit spacer before it).

### 17.2 Action pair

Two sibling action intents in a row:

- Rendered in a right-aligned row at the content tail.
- Left button = outline (secondary).
- Right button = filled primary.
- Reference gap: 12 px column gap.
- Order is **declaration order**; the right-most is always the primary.
- Reference labels: if one is labelled any of `"Cancel"`, `"Back"`, `"Discard"`, `"Skip"`, `"Later"` (case-insensitive), the bridge treats it as secondary regardless of declaration order — the right-most non-secondary becomes primary.

### 17.3 Three or more actions

Three or more sibling actions:

- The bridge collapses the non-primary ones into a Choice Overlay (reference: a menu button to the left of the primary).
- OR stacks them vertically in declaration order with the last promoted. Bridge discretion based on available width.

### 17.4 Action inside a list row

An `trigger` inside a `list` item (e.g., per-row "Delete"):

- Rendered inline with the item content, right-aligned.
- Never promoted to primary.
- If the trigger is a `confirm`, the confirm dialog references the item's context (bridge inspects the parent list item's data to compose the prompt title).

### 17.5 Enum picker

Semantically, a `choice` with a small, known option set (`options.len() ≤ 4`) **may** render inline as a row of outline buttons instead of triggering the overlay (spatial substrate only). This is a bridge optimisation; the intent itself is unchanged — activation still fires `on change`. For wider option sets the overlay is mandatory.

### 17.6 Destructive emphasis

A `confirm` whose label contains any of `"DELETE"`, `"REMOVE"`, `"KILL"`, `"FORMAT"`, `"RESET"` (case-insensitive) renders its OK button in a destructive colour (reference: `#FF3333` fixed across themes — intentionally theme-invariant so the cue is universal).

### 17.7 `navigate` badge

`navigate` with `badge: :some n` (n > 0) renders a badge circle in the row's trailing position (before the disclosure indicator). `badge: 0` or absent renders nothing.

### 17.8 `create` vs `navigate`

Both take the user elsewhere. Reference bridges distinguish:

- `create` suggests "new thing starts here" — render with a `+` glyph prefix or as a FAB.
- `navigate` suggests "move to existing thing" — render with disclosure affordance `›`.

## 18 · Inline vs modal-context

Certain intents route to a **modal context** (§22) — an exclusive bridge-owned interaction — instead of appearing inline in the content. The decision is universal; the rendering of "inline" vs "modal" is substrate-specific (inline = in-content widget on spatial / part of the same voice turn on voice; modal = overlay on spatial / dedicated turn on voice).

| Intent / node | Inline | Modal context |
|---|---|---|
| `confirm` | never | always (Confirm Dialog) |
| `choice` | sometimes (§17.5, spatial only) | activation-to-open (Choice Overlay) |
| `multiselect` | never | activation-to-open (Choice Overlay, multi mode) |
| `date` | never | activation-to-open (Date Picker) |
| `share` | trigger-like inline | activation-to-open (Share Sheet) |
| `text` / `password` / `search` | inline input affordance | keyboard is modal (§29) when focused (visual-input substrates only) |
| `pin` | inline numpad (spatial) / sequential prompt (voice) | — |
| `loading` (root) | — | always (Loading) |
| `loading` (nested) | inline indicator | — |
| `progress` (root) | — | always (Progress) |
| `progress` (nested) | inline indicator | — |
| First-use grant prompt | — | Permission Dialog |
| Lockscreen (on lock) | — | Lockscreen (exclusive) |
| Back-confirm (`@on back :confirm`) | — | Confirm Dialog |

## 19 · Adaptation: form factor, density, rotation

*Substrate scope: spatial substrates. Voice bridges adapt pacing and verbosity instead — see §12bis.*

The bridge consults its own substrate state (current orientation, pixel density, viewport geometry) when choosing presentation metrics. These values are **not** part of the content snapshot (§7) — they are bridge-internal properties resolved from the display / panel driver. Apps never see either.

**Reference adaptation table** (CyberDeck, 800 × 480):

| Axis | Portrait (480 × 800) | Landscape (800 × 480) |
|---|---|---|
| Statusbar | Bottom-bordered, 36 px | Right-bordered, 36 px |
| Navbar | Bottom docked, 60 px tall | Right docked, 60 px wide |
| List row height | 56 px | 56 px |
| Launcher grid columns | 3 | 5 |
| Markdown `:reading` measure | 480 − 32 pad | min(740, 640) |
| Toast offset | Up by navbar/2 | Left by navbar/2 |

**Other form factors (future / hypothetical):**

| Form factor | Layout adaptation example |
|---|---|
| Smartwatch (240 × 240) | One card per screen; `list` paginates one item per page; statusbar hidden; navbar replaced by crown scroll. |
| E-ink 600 × 800 | No animations; 1.2× line height; monochrome-safe palette. |
| Voice assistant | Turn-taking: each content tree = one turn; inputs asked one at a time. |
| Terminal 80 × 24 | Box-drawn borders; colour via ANSI; inputs via readline. |

## 20 · Invariants across all bridges

Regardless of substrate, a bridge MUST preserve:

- **Intent completeness** — every non-decorative content node eventually reaches the user through some affordance. The bridge may not drop an intent because it doesn't fit.
- **Ordering** — children of `list`, `group`, `form` render in declaration order. A voice bridge reads them in order; a screen bridge stacks them in order.
- **Identity** — `(app_id, machine_id, state_id)` identifies a single "page". The bridge never renders two different states of the same machine simultaneously.
- **One-modal-at-a-time** — at most one overlay (dialog, keyboard, choice overlay, date picker, share sheet, permission dialog) is active. A new overlay replaces the old one (bridge chooses replacement or queue policy; reference: replace).
- **Crash containment** — bridge panics do not leak to apps; app panics do not crash the bridge.
- **No side channels** — the bridge never reads app state it wasn't handed. It has no backdoor into the app's `@machine`, `@config`, or `@private`.
- **No render-driven state change** — the act of presenting a snapshot fires no intents and mutates no state. Intents fire only on genuine user activation or on bridge-owned resolution services returning a user-supplied value. A patched update of an input's displayed `value:` does NOT fire that input's change handler (§6.1.4). This is what keeps the unidirectional pipeline from looping (§6.1).

---

# Part IV — UI services

The bridge provides a set of **UI services** — autonomous overlays and subsystems that are NOT part of the content tree. Apps never call them directly. They are triggered by:

1. **Semantic content** (e.g., a `confirm` intent triggers the Confirm Dialog),
2. **System-service calls** (e.g., `notify.toast(…)` triggers the Toast Service),
3. **Runtime lifecycle events** (e.g., `@on launch` triggers Loading Overlay; `@on back :confirm` triggers the Confirm Dialog).

UI services are not themselves services in the `DECK-3.0-SERVICES.md` sense. They are bridge-internal subsystems that **back** system services.

## 21 · Catalog

The catalog below distinguishes **resolution services** — the user-interaction concept every bridge must resolve in its own substrate — from **ambient indicator services** — persistent surfaces that only exist when the substrate can dedicate room to them. The latter are not universal.

| # | Service | Kind | Scope | Triggered by | Section |
|---|---|---|---|---|---|
| 1 | Statusbar | ambient indicator | spatial + dedicated strip | bridge always-on | §23 |
| 2 | Navbar | ambient indicator | spatial + tap-addressable regions | bridge always-on | §24 |
| 3 | Toast | resolution (announcement) | universal | `notify.toast()`, bridge internals | §25 |
| 4 | Confirm Dialog | resolution (binary) | universal | `confirm` intent, `@on back :confirm`, bridge destructive actions | §26 |
| 5 | Loading | resolution (wait, indeterminate) | universal | root `loading` node, `@on launch` window | §27 |
| 6 | Progress | resolution (wait, measurable) | universal | root `progress` node, cancellable action streams | §28 |
| 7 | Keyboard | resolution (text entry) | visual-input (touch + framebuffer) | focused text/password/search input | §29 |
| 8 | Choice / Multiselect | resolution (value from list) | universal | tapped `choice`/`multiselect`, bridge choose-one actions | §30 |
| 9 | Date Picker | resolution (date value) | universal | tapped `date` intent | §31 |
| 10 | Share Sheet | resolution (target selection) | universal | tapped `share` intent | §32 |
| 11 | Permission Dialog | resolution (consent) | universal | first-use capability grant | §33 |
| 12 | Lockscreen | exclusive modal | universal | `security.lock()`, auto-lock timer, boot lock | §34 |
| 13 | Badge | ambient indicator | spatial | `badge:` attribute, `apps.notif_counts_watch()` stream | §35 |

**Reading the scope column.** *Universal* means every bridge implements some presentation of the service in its substrate — a voice bridge speaks the confirm prompt; a terminal bridge opens a readline; a framebuffer bridge draws an overlay. The SEMANTIC contract is identical across substrates. *Spatial* means the service only exists on substrates with a 2-D presentation surface; bridges lacking such a surface stub the vtable method to `:not_supported`, and upstream system services surface that as `:err :unavailable`.

## 22 · Modal context

A **modal context** is a bridge-owned user interaction that takes precedence over the ambient content until it resolves. The concept is universal; the rendering is substrate-specific.

| Substrate | How modal context is expressed |
|---|---|
| `:framebuffer` / `:eink` | Z-layer above the active screen (the "overlay layer") with backdrop dimming where supported. |
| `:voice` | A dedicated turn — the modal prompt is spoken; the bridge refuses to emit further content updates until the user responds. |
| `:terminal` | Readline-style prompt at the bottom of the grid; content above is frozen until resolution. |

**Mutual exclusion (universal):**

- At any moment, at most one modal context is active. A new modal replaces the current one (reference policy; bridges may queue if their substrate requires it).
- Non-modal announcements (Toast) coexist with modal contexts; at most one announcement at a time (new replaces old).
- Badge decoration is not a modal context — it is an in-content annotation.

**Reference spatial rendering** (framebuffer):

```
┌──────────────────────────────────────────────────┐
│  Overlay layer                                    │  ← modal context: dialog, keyboard,
│  (at most one modal + at most one announcement)   │    choice, date, share, permission,
│                                                   │    lockscreen, loading, progress;
│                                                   │    non-modal: toast
├──────────────────────────────────────────────────┤
│  Active screen                                    │
│    Ambient persistent indicators (§23 if visual)  │
│    Content area                                   │
│    Ambient navigation (§24 if visual)             │
├──────────────────────────────────────────────────┤
│  Background                                       │  ← never visible in normal operation
└──────────────────────────────────────────────────┘
```

**Dismissal order** (critical for callbacks):

1. Free bridge-owned resources (canvas buffers, timers).
2. Tear down the overlay widget tree.
3. Invoke the completion callback (which may push a new activity or fire an intent).

The callback is last because it may immediately trigger a state change that pushes the next activity; invoking it before tearing down causes the underlying presentation state to dangle.

## 23 · Statusbar

*Substrate scope: spatial substrates that can dedicate a strip of output to persistent ambient indicators (`:framebuffer`, `:eink`, `:terminal` with ≥ 2 rows spare). Voice and headless bridges do not implement; `set_statusbar` returns `:err :not_supported`. A voice bridge surfaces the same underlying streams through the `"status"` voice command.*

A persistent indicator strip at the leading edge of the active screen.

### 23.1 Trigger

Always on (reference), except when:

- Lockscreen is active (§34 explicitly hides it).
- The app declared `@app.fullscreen: true` (future, not in core spec).

### 23.2 Content

Driven by system-service streams the bridge subscribes to on boot:

| Slot | Source | Update |
|---|---|---|
| App title | `system.apps.info(current).name` | On activity change |
| Time | `time.now()` | Every 60 s |
| WiFi | `network.wifi.status_watch()` | On emission |
| Battery | `power.watch()` | On emission |
| BT | `network.bluetooth.status_watch()` | On emission |
| SD | `system.events.subscribe("os.storage_changed")` | On emission |
| Theme | `system.theme.watch()` | On emission (triggers rebuild) |

### 23.3 Visual (reference)

Portrait:

```
┌───────────────────────────────────────────────────┐
│ /APP TITLE\  TIME  BT  SD  WIFI  BATT%           │  36 px, 2 px dim bottom border
└───────────────────────────────────────────────────┘
```

- App title: canvas-drawn parallelogram in `primary_dim` fill, `bg_dark` text, MD ALL CAPS.
- Icons are bridge-drawn primitives (canvas triangle, filled circles) — not font glyphs.
- Battery: outlined box + fill bar + "XX%" label.
- WiFi: 4 vertical bars of increasing height, colour-coded by signal.

### 23.4 Rules

- Apps MUST NOT write to the statusbar. `@app.name` is the only app-authored identity the statusbar displays.
- The statusbar is never part of the content tree.
- System events trigger stream updates; the bridge diff-patches the relevant slot without rebuilding the full statusbar.

## 24 · Navbar

*Substrate scope: spatial substrates with **discrete tap-addressable regions** (`:framebuffer` + `:touch` or `:framebuffer` + `:remote`). E-ink + physical-buttons implement it as button-mapped labels. Terminal bridges use keyboard shortcuts (no visual bar). Voice bridges route BACK/HOME/TASK-SWITCH to hotwords, no visual bar. The BACK/HOME/TASK-SWITCH signals themselves are **universal** (§24.3) — only the affordance is substrate-specific.*

A persistent navigation control at the trailing edge of the active screen on touch-input bridges.

### 24.1 Trigger

Always on for `:touch` substrates, unless:

- Lockscreen is active.
- `@app.fullscreen: true` (future).

Not rendered on voice, terminal, or headless bridges — those handle BACK/HOME/TASK-SWITCH out of band (voice command, keyboard shortcut, remote button).

### 24.2 Buttons (reference)

Three zones, equal width:

| Button | Glyph (reference) | Action |
|---|---|---|
| Back | Left-pointing triangle (canvas) | Runtime fires `@on back` for the top app |
| Home | Filled circle | Runtime suspends every app above the launcher |
| Tasks | Filled square | Short tap = task switcher; long tap (>600 ms) = Task Manager |

All glyphs are geometric primitives (canvas), not font icons — matches the terminal aesthetic of the reference theme.

### 24.3 `@on back` round-trip

1. User taps BACK (or performs the swipe-down gesture, §39).
2. Bridge is **not** allowed to pop the activity directly; it fires the runtime's "back" signal.
3. Runtime runs the top app's `@on back`:
   - Returns `:handled` → no navigation. The app dismissed something internally (closed a submenu via `to previous`, etc.).
   - Returns `:unhandled` → runtime instructs the bridge to pop the activity (suspend-to-home, unless memory pressure forces pop-to-home).
   - Returns `:confirm (prompt, confirm: (label, atom), cancel: (label, atom))` → bridge presents a Confirm Dialog (§26) with the supplied strings; the selected atom is sent back to the runtime as the final `BackResult`.

### 24.4 Nav lock

The bridge supports a **nav lock** flag. When set:

- BACK and HOME buttons are visually dimmed and swallow their gestures.
- TASKS still works (user can switch to Task Manager for emergency kill).
- Set by the Lockscreen (§34).
- Set by apps declaring `@app.fullscreen: true` (future).

## 25 · Toast Service

### 25.1 Trigger

- `notify.toast(message, duration?)` from `system.notify` (§31 of SERVICES).
- Bridge-internal announcements ("Coming soon…" for stubbed apps, `@on back :confirm` rejection, etc.).

### 25.2 Spec struct

```c
typedef struct {
  const char *app_id;         /* who posted; may be NULL for bridge-internal */
  const char *message;        /* UTF-8 */
  uint32_t    duration_ms;    /* 0 = default (2000) */
  uint8_t     level;          /* 0 = info, 1 = success, 2 = warning, 3 = error */
} DeckToastSpec;
```

### 25.3 Substrate renderings

**Spatial (reference — framebuffer):**

- Centred in the content area; in portrait shifted up by `navbar/2`; in landscape shifted left by `navbar/2`.
- Fill: `bg_card` (`#0A0A0A`).
- Border: 1 px `primary` (or theme's `accent` when `level == 3 error`).
- Radius 2 px, pad 12 h / 6 v.
- Text: SM, primary colour (non-caps — the sole reference-theme exception to ALL-CAPS rule).
- Fade-in 150 ms; no fade-out.

**Voice:** spoken as a short interjection between turns, preceded by a soft earcon. Never interrupts an active modal prompt — queued until the modal resolves, or dropped if newer toasts arrive first (same "newest-wins" policy).

**Terminal:** printed on a dedicated status line (reference: last row of the grid), cleared after `duration_ms` via `lv_timer` or equivalent. Non-blocking.

### 25.4 Durations (reference)

| Level | Default |
|---|---|
| info | 2000 ms |
| success | 1200 ms |
| warning | 2500 ms |
| error | 2500 ms |

### 25.5 Queue behaviour

No queue. A new toast immediately dismisses the previous. This matches the demo's reference behaviour — toast is a transient acknowledgement, not a log.

## 26 · Confirm Dialog Service

### 26.1 Trigger

- `confirm` intent tapped.
- `@on back` returned `:confirm (…)` (§14.8 of DRAFT).
- System services that need user consent (`system.tasks.kill()` escalation, bridge-owned destructive actions like "Clear cache?").

### 26.2 Spec struct

```c
typedef struct {
  const char *title;          /* may be empty */
  const char *prompt;         /* body text; MD typography */
  const char *ok_label;       /* e.g., "DELETE" */
  const char *cancel_label;   /* e.g., "CANCEL" */
  bool        destructive;    /* OK button styled destructive if true */
  void      (*on_resolve)(bool confirmed, void *user);
  void       *user;
} DeckConfirmSpec;
```

### 26.3 Visual (reference)

```
┌─────────────────────────────────────────────────────┐
│  (semi-transparent backdrop, LV_OPA_50)             │
│                                                     │
│    ┌───────────────────────────────────────────┐    │
│    │▓▓▓ TITLE ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓/  │    │  ← parallelogram 28 px
│    │  Prompt text in MD primary.              │    │
│    │  Wraps across lines.                     │    │
│    │                                          │    │
│    │                      [CANCEL]    [OK]    │    │  ← action row, 24 px above
│    └───────────────────────────────────────────┘    │
│     380 px wide, bg_dark, 1 px dim border, radius 2 │
└─────────────────────────────────────────────────────┘
```

- Title polygon: bridge-drawn, matches statusbar shape.
- Body: `pad_all=24`, `pad_row=16`.
- Prompt is the **protagonist** — MD primary, not dim. Title is context.
- Action row: CANCEL (outline) left, OK (primary or destructive) right, 8 px column gap.
- `destructive: true` → OK button in destructive colour (§17.6).

### 26.4 Rules

- If `title` is empty, the polygon row is omitted.
- Tapping OK → `on_resolve(true, user)`.
- Tapping CANCEL or backdrop → `on_resolve(false, user)`.
- For `confirm` intent, OK confirmation fires the intent's post-confirmation `->` action with no payload. CANCEL fires nothing.
- For `@on back :confirm`, each choice returns the atom the app declared.

## 27 · Loading Overlay

### 27.1 Trigger

- Root-level `loading` node in the content tree.
- The runtime's `@on launch` window: from VM load until the first non-loading render. Bridge auto-shows; auto-hides.
- Explicit `loading_show(label)` from system services during long operations where no progress is measurable.

### 27.2 Spec

```c
void loading_show(void *h, const char *label);   /* label may be NULL */
void loading_hide(void *h);
```

### 27.3 Substrate renderings

**Spatial (reference — framebuffer):** full-screen backdrop at `LV_OPA_70`; centred blinking `_` cursor in XL typography, primary colour; blink interval 500 ms; optional label below cursor in SM dim.

**Voice:** periodic "working…" utterance every ~3 s until dismissed; optional label spoken once on show.

**Terminal:** single-char spinner cycling in place at cursor position.

### 27.4 Dismissal

Auto-dismissed when the next `push_snapshot` arrives whose root is not `loading`. Bridge detects by inspecting the root node's kind.

## 28 · Progress Overlay

### 28.1 Trigger

- Root-level `progress` node in the content tree.
- Explicit `progress_show(spec)` from a system service (e.g., OTA download).

### 28.2 Spec struct

```c
typedef struct {
  const char *title;
  bool        cancellable;
  void      (*on_cancel)(void *user);
  void       *user;
} DeckProgressSpec;

void progress_set(void *h, float pct);  /* 0..1 for determinate, -1 for indeterminate */
void progress_hide(void *h);
```

### 28.3 Substrate renderings

**Spatial (reference — framebuffer):** full-screen backdrop at `LV_OPA_70`; title centred, MD primary; progress bar below (20 px height, 1 px dim border, primary fill); determinate fills proportionally with centred "XX%"; indeterminate renders a 45° zebra ribbon animated 2 px/frame (primary_dim / bg_dark stripes); CANCEL button below bar (outline) if `cancellable: true`, 24 px gap.

**Voice:** title spoken once on show; percentage announced every ~10 s if determinate, every ~15 s if indeterminate ("still working"); CANCEL prompt offered on silence ("say 'cancel' to abort").

**Terminal:** bar rendered as `[=======>   ] 63%` in the last row; CANCEL via Ctrl-C if cancellable.

### 28.4 Rules

- Indeterminate → determinate transition allowed mid-operation (e.g., HEAD request returned Content-Length).
- Cancel fires `on_cancel`; the app decides whether to abort the operation.
- Progress overlay is NOT dismissed automatically by `progress_set`; only by `progress_hide` or by a new snapshot whose root is not `progress`.

## 29 · Keyboard Service

*Substrate scope: visual-input substrates that need an on-screen soft keyboard (`:framebuffer` + `:touch`). Physical-keyboard and voice bridges stub `keyboard_show/hide` to `:not_supported`; they satisfy text intents through their native input (readline on terminal, speech recognition on voice).*

### 29.1 Trigger

- A `text` / `password` / `search` / `markdown_editor` input is present in the active screen AND is focused.

### 29.2 Policy

- Show only **on resume** — never on initial `push_snapshot` of an activity. This prevents the keyboard from flashing up during screen transitions.
- Hide when:
  - Focus leaves the input (tap outside).
  - Keyboard's own Confirm (OK) is tapped — fires `on change` / `on complete` / `on submit` with the current textarea value.
  - The runtime pushes a new snapshot for a different state.

### 29.3 Spec

```c
typedef enum {
  DECK_KBD_TEXT_UPPER,
  DECK_KBD_TEXT_LOWER,
  DECK_KBD_NUMERIC,
  DECK_KBD_EMAIL,
  DECK_KBD_URL,
  DECK_KBD_PASSWORD,   /* same as TEXT_UPPER but suppresses autocomplete */
} DeckKeyboardKind;

void keyboard_show(void *h, DeckKeyboardKind k);
void keyboard_hide(void *h);
```

### 29.4 Visual (reference — framebuffer)

- LVGL `lv_keyboard`, docked to bottom, ~40 % of viewport height.
- Associated textarea scrolls to stay visible above the keyboard.
- Themed per reference palette.

### 29.5 PIN note

`pin` intent does NOT use the keyboard service. It uses an inline custom numpad (§16.1) rendered as part of the content tree. This is because PIN entry has semantic constraints (fixed length, digits only, no autocomplete) that the general keyboard cannot enforce.

## 30 · Choice / Multiselect Overlay

### 30.1 Trigger

- Tapped `choice` intent (single).
- Tapped `multiselect` intent (multi).
- Tapped `share` intent (re-used internally for share target selection).

### 30.2 Spec struct

```c
typedef struct {
  const char *field_name;           /* shown as overlay title */
  const char * const *labels;
  const char * const *value_atoms;  /* atom per option */
  uint32_t    n_options;
  int32_t     selected_idx;         /* for single; -1 if none */
  const bool *selected_mask;        /* for multi; length == n_options */
  bool        multi;
  void      (*on_resolve_single)(int32_t idx, void *user);
  void      (*on_resolve_multi) (const bool *mask, void *user);
  void       *user;
} DeckChoiceSpec;
```

### 30.3 Substrate renderings

**Spatial (reference — framebuffer):** full-screen-width sheet, up to 80 % viewport height; title bar: field name in dim SM; option rows: `●` (bullet) selected, `-` (hyphen) unselected, label MD primary; tap a row — single mode fires `on_resolve` + dismisses; multi mode toggles; multi mode adds an action row at the bottom: `[CANCEL] [CONFIRM]`.

**Voice:** title spoken; options enumerated ("1: Option A. 2: Option B. …"); user says a number (single) or space-separated numbers (multi), then "confirm"; timeout → re-prompt once then cancel.

**Terminal:** numbered list; user types a digit (single) or comma-separated digits (multi); Enter confirms, Esc cancels.

### 30.4 Rules

- Empty options list: nothing to pick; overlay not presented; intent silently drops.
- Cancel fires nothing (consistent with other cancellable overlays).

## 31 · Date Picker

### 31.1 Trigger

- Tapped `date` intent.

### 31.2 Spec struct

```c
typedef struct {
  const char *field_name;
  int64_t     initial_ts_ms;
  int64_t     min_ts_ms;      /* 0 = unbounded */
  int64_t     max_ts_ms;      /* 0 = unbounded */
  void      (*on_resolve)(int64_t ts_ms, bool confirmed, void *user);
  void       *user;
} DeckDateSpec;
```

### 31.3 Substrate renderings

**Spatial (reference — framebuffer):** month/year header with prev/next triangles; 7-column day grid (Mon–Sun headers dim SM, day numbers MD primary); selected day filled primary with text bg_dark; disabled days (outside range) dim; action row `[CANCEL] [CONFIRM]`.

**Voice:** "What year?" → "What month?" → "What day?" sequential; locale-formatted re-readback for confirmation ("April 23, 2026. Correct?"); user says "yes" or offers a new date.

**Terminal:** readline prompt formatted `YYYY-MM-DD: `; validated on submission against min/max.

### 31.4 Result

Return timestamp is **local midnight** of the chosen day, in ms since epoch. Apps that need time-of-day precision use separate `range` intents or a typed `text` input with ISO parsing.

## 32 · Share Sheet

### 32.1 Trigger

- Tapped `share` intent.

### 32.2 Spec struct

```c
typedef struct {
  const char *preview;             /* short description for the sheet title */
  const char *payload_kind;        /* atom string: "text" / "url" / "image" / ... */
  const char *payload_str;         /* str payload when payload_kind in {text,url} */
  /* opaque byte payloads reach the share target via share.target service's IPC */
  const char * const *targets;     /* candidate app IDs (discovered by bridge) */
  const char * const *target_labels;
  uint32_t    n_targets;
  void      (*on_resolve)(const char *target_id, bool sent, void *user);
  void       *user;
} DeckShareSpec;
```

### 32.3 Visual (reference)

- Choice Overlay style with rows listing targets.
- Each target label comes from the target app's `@app.name`.
- Additional built-in targets: `"Copy"` (clipboard), `"Save to file"` (if `fs` grant available).

### 32.4 Dispatch

Bridge invokes `share.target` service (SERVICES §44) with the chosen target ID and the payload. The user-selected target app receives a cold-spawn IPC invocation with the shared content.

## 33 · Permission Dialog

### 33.1 Trigger

- An app's first call to a capability whose `@grants.services.<alias>.prompt` is `:on_first_use`.

### 33.2 Spec struct

```c
typedef struct {
  const char *app_name;       /* requester */
  const char *service_id;     /* what they want */
  const char *reason;         /* from @grants */
  void      (*on_resolve)(bool granted, void *user);
  void       *user;
} DeckPermissionSpec;
```

### 33.3 Visual (reference)

- Confirm Dialog with title `"PERMISSIONS"`, body formatted as:
  `"{app_name} wants to use {service_id}.\n\n{reason}"`.
- Actions: `[DENY] [ALLOW]`.
- DENY fires with `granted: false`; the capability call returns `:err :permission_denied`.
- ALLOW fires with `granted: true`; the call proceeds, and the grant is cached.

### 33.4 Rules

- Atomically one permission prompt at a time. Further first-use prompts queue.
- On ALLOW the grant is persisted by `system.security` per the app's declared `persist:` policy.
- On DENY the app may re-prompt on a future call (subject to `prompt:` policy); the bridge is not aware of rate limiting — that's in `system.security`.

## 34 · Lockscreen

### 34.1 Trigger

- Boot, if `sec.pin_set() == true`.
- Explicit `security.lock()` call from a system app (Settings).
- Auto-lock timer fires (`display.watch()` reporting `locked: true`).
- Low-battery policy (`power.watch()` crossing a platform-configurable threshold).

### 34.2 Rendering

The lockscreen is **not** a Deck app. It is a bridge-owned exclusive modal that supersedes all content until `sec.verify_pin` succeeds. Every active content snapshot, every overlay, every statusbar/navbar-like affordance is suppressed from the user's perception while the lockscreen is active.

**Spatial (reference — framebuffer):** the bridge pushes a bridge-owned activity that hides the statusbar and navbar and sets the nav lock (§24.4). Layout:

```
  ┌──────────────────────────────────────┐
  │                                      │
  │     ● ● — —       ← dot row, XL      │
  │                                      │
  │     [1] [2] [3]                      │
  │     [4] [5] [6]   ← numpad 3×4       │
  │     [7] [8] [9]                      │
  │     [←] [0] [✓]                      │
  │                                      │
  └──────────────────────────────────────┘
```

**Voice:** bridge refuses to speak anything except "Device locked. Say your PIN." and its re-prompt. Digits are spoken one at a time; "confirm" finalises; "clear" resets. The device does not respond to any other command while locked.

**Terminal:** a blocking readline at a blank screen prompts `PIN: ` with echo suppressed; Enter submits.

### 34.3 Validation flow

1. User enters digits.
2. On OK (`✓`) the bridge calls `sec.verify_pin(pin)`.
3. On `:ok true` → bridge pops the lockscreen, clears nav lock, restores statusbar + navbar.
4. On `:ok false` → bridge flashes the dot row (red pulse 200 ms), clears the entry, posts `"INCORRECT PIN"` toast.
5. On `:err e` → posts `"PIN UNAVAILABLE"` toast; lockscreen remains.

### 34.4 Rules

- The lockscreen is the **only** bridge subsystem that cannot be dismissed by any ambient navigation signal (BACK / HOME / TASK-SWITCH, in whatever form the substrate provides them).
- The lockscreen survives every bridge-internal event that would otherwise cause a rebuild (rotation, theme change, resume, etc.) — it reasserts itself.
- The lockscreen never exposes any app's content, not even a statusbar preview.

## 35 · Badge Service

*Substrate scope: spatial substrates. Voice bridges convey badge counts by prepending the count when enumerating an item ("three new notifications: …"). Terminal bridges prefix the count inline.*

### 35.1 Trigger

- A content node of kind `:trigger` / `:navigate` / `:create` with non-zero `badge:` attribute.
- Launcher app grid cells (badges come from `apps.notif_counts_watch()` stream).

### 35.2 Visual (reference)

- Small rounded-rect circle at the widget's top-right corner.
- Fill: primary.
- Text: bg_dark, SM.
- Width auto-sized to digit count (min 20 px).
- Values ≥ 100 render as `"99+"`.

### 35.3 Rules

- Badge = 0 or absent → no badge rendered.
- Badge is a pure decoration; tapping the badge is equivalent to tapping its host widget.
- Badge updates are patch-eligible — the bridge does not rebuild the host widget to change the badge count.

---

# Part V — Subsystems

Each subsystem is marked with its substrate scope. A bridge only implements the ones its substrate supports; the rest stub to `:not_supported` at the vtable level.

| Subsystem | Scope | Section |
|---|---|---|
| Rotation | Physical-display | §36 |
| Theme | Universal (atom passthrough; substrate-specific expression) | §37 |
| Brightness & screen power | Physical-display; quiescent analogue universal | §38 |
| Gesture processing | Touch input; ambient-navigation signals universal | §39 |
| Input routing | Universal | §40 |
| Fonts & glyph fallback | Spatial substrates | §41 |
| Asset resolution | Universal | §42 |

## 36 · Rotation

*Substrate scope: **physical-display** bridges only — substrates that own a pixel-controllable panel (`:framebuffer`, `:eink`). Voice, terminal, and headless bridges stub `set_rotation` to `:not_supported`; `system.display.set_rotation` then returns `:err :unavailable` on those platforms.*

### 36.1 Model

A bridge supporting rotation exposes one or more orientations to the device. The reference CyberDeck bridge supports two (`:portrait` 480×800, `:landscape` 800×480); other substrates may support none or four.

`system.display.set_rotation(r)` → bridge:

1. Applies the rotation to its panel driver.
2. Re-resolves its internal presentation context (viewport geometry, metrics).
3. Rebuilds every active activity without the runtime having re-sent any snapshot — the last-known snapshot is replayed internally by the bridge.
4. Re-arms any transient overlays (keyboard focus, modal position) as needed.

### 36.2 Rules

- Apps may declare `@app.orientation: :portrait | :landscape` as an identity field. The bridge suppresses rotation attempts while that app is foregrounded. Bridges without rotation support silently ignore the field.
- Input-coordinate rotation is the panel driver's concern, not the bridge's — the bridge consumes already-transformed coordinates from `display.touch` or equivalent.
- Rotation **never** reaches the runtime. The runtime does not know whether the device rotates. Apps do not learn of rotation via any content-level signal — they learn through the snapshot-independent rebuild that the bridge drives.

## 37 · Theme

### 37.1 Model

`system.theme.current()` returns the active `Theme` (§32 of SERVICES). The bridge subscribes to `theme.watch()` at boot. On emission:

1. Bridge updates its internal palette reference.
2. Every active activity is rebuilt (same mechanism as rotation).
3. Toast announces the change ("THEME: NEON").

### 37.2 Rules

- Reference bridge ships three themes: `:green` (Matrix), `:amber` (Retro), `:neon` (Cyberpunk).
- A theme may not add or remove semantic vocabulary — it only changes palette and, optionally, typography. Layout metrics are theme-invariant.
- Apps do not choose themes. Settings app chooses; `system.theme.set(id)` is system-only.
- Apps that need theme-derived colours for non-widget purposes (custom chart palette) subscribe to `theme.watch()` themselves. Most apps don't need this — the bridge has already rebuilt their screens.

## 38 · Brightness & screen power

*Substrate scope: **physical-display** bridges only. Voice, terminal, and headless bridges stub `set_brightness` to `:not_supported`. Sleep / wake semantics are universal (every bridge can enter a quiescent state and resume), but the means of expressing them vary: a voice bridge goes quiet on sleep, a terminal bridge stops redrawing.*

### 38.1 Model (physical-display)

`system.display.set_brightness(level)` → bridge calls the display panel driver's `set_brightness(level)`. No content-tree effect.

`system.display.sleep()` / `wake()` → bridge calls panel's `set_on(false/true)`. On sleep, bridge pauses its rendering scheduler; on wake, resumes and forces a full rebuild of the top activity.

**Quiescent analogues on non-visual substrates:**

- Voice bridges enter quiescent mode on sleep (no spoken output, ASR paused); wake resumes with a short earcon.
- Terminal bridges halt output on sleep; wake redraws from current snapshot.

### 38.2 Auto-dim policy

The bridge does NOT implement auto-dim. That belongs to a system app (Settings / Power) that calls `display.set_brightness` on timers. The bridge only executes the transition.

## 39 · Gesture processing

*Substrate scope: `:touch` input substrates. Physical-keyboard, remote, and voice bridges map the **ambient navigation signals** BACK / HOME / TASK-SWITCH to their native input (hotkey, button, hotword) — the signals themselves are universal; only the detection layer is substrate-specific.*

### 39.1 Model (touch)

On touch substrates, the bridge intercepts raw touch events before the widget library sees them. Gestures defined at bridge level:

| Gesture | Condition | Action |
|---|---|---|
| HOME | Swipe up from bottom edge (≥ 60 px in the 80-px strip) | Equivalent to tapping navbar HOME |
| BACK | Swipe down from top edge (≥ 60 px in the 80-px strip) | Equivalent to tapping navbar BACK |
| TASKS | Long press (> 600 ms) on navbar HOME | Launch Task Manager |
| App-settings quickjump | Long press on launcher grid cell | Launch Settings → Apps → {app_id} |

Gestures matched at this layer are **not** forwarded to the widget library. Unmatched touches pass through normally.

### 39.2 Nav lock

When nav lock is set (§24.4), HOME and BACK gestures are swallowed unconditionally. Widget library never sees them. Only TASKS still works.

### 39.3 Rules

- Gesture zones are bridge policy, not language policy. A bridge for a different form factor chooses different zones (e.g., crown rotation on a watch).
- Gestures never carry app-visible payloads; they translate into fixed system events (HOME, BACK, TASK-SWITCH) that the runtime dispatches.

## 40 · Input routing

Every bridge is responsible for routing raw user input to the right consumer: the content, the active modal context, or the ambient navigation layer. The routing table below is written for the spatial + touch reference bridge; the principle is universal — a voice bridge routes ASR utterances between the current modal prompt, the content's focused input, and hotword-mapped navigation signals — but the specific event sources differ.

**Reference table (spatial + touch):**

| Event source | Bridge behaviour |
|---|---|
| Touch on content affordance | Content library handles; intent fires on commit |
| Touch on modal overlay | Overlay handles; possibly dismisses |
| Touch on statusbar/navbar | Bridge itself handles (doesn't reach content library) |
| Keyboard key | Keyboard service consumes; Confirm key fires intent; backdrop tap dismisses |
| External hardware key (power button, etc.) | Bridge maps to system events (`system.events.publish`) or directly to power/display actions |

**Voice-substrate analogue:** every user utterance is routed to (a) the active modal prompt if any, (b) the focused content input if any, (c) the hotword detector otherwise. Navigation hotwords ("home", "back") fire the ambient signals.

## 41 · Fonts & glyph fallback

*Substrate scope: spatial substrates that render text as glyphs (`:framebuffer`, `:eink`, `:terminal`). Voice substrates have no glyph concerns — they handle the analogous problem, TTS pronunciation of unknown words, through their speech engine, not through this subsystem.*

Bridge fonts on spatial substrates are platform-specific. The CyberDeck reference is limited to Montserrat plus LVGL's built-in symbol glyph range. Characters outside these ranges render as `?` or rectangles.

**Rules:**

- Apps never specify fonts.
- Text that contains non-Latin characters renders best-effort; the app declared content, not typography — the bridge is responsible for choosing a font that covers the glyphs it ships with, and for gracefully degrading unsupported codepoints.
- Symbol characters in app text (`*`, `→`, `●`, etc.) are **not** converted. Apps should write plain text; the bridge chooses decorative glyphs internally (e.g., `navigate` renders with `›`, but the app writes only the label).

## 42 · Asset resolution

The content tree may reference assets via:

- `media expr alt:` where `expr` is a path string or an `AssetRef`.
- `markdown` content containing `![alt](path)`.
- `@assets`-declared icons referenced implicitly by `@app.icon:`.

**Resolution:**

- `AssetRef` → bridge calls `assets.resolve(ref)` to get a platform path; loads into widget library.
- Plain string path → bridge treats as local `fs` path.
- Network URLs — NOT supported by reference bridge (see §15.2). Apps cache via `data.cache` / `storage.cache` and pass local paths.

**Ready/not-ready:** for `AssetRef` with `ready: false` (downloadable asset not yet cached):

- `media` → bridge renders spinner + alt text; re-renders when ready.
- Other consumers handle their own fallback per `DRAFT.md §20`.

---

# Part VI — System-service integration

The bridge hosts the UI backend for several `system.*` services. This section specifies the contract between each service and the bridge.

## 43 · `system.display`

Bridge contract is split by scope. A bridge that does not support a visual-only method returns `:not_supported` from the vtable; `system.display` propagates as `:err :unavailable` to callers.

**Universal** (every bridge implements):

| Service method | Bridge action |
|---|---|
| `display.lock()` | Present Lockscreen (§34) |
| `display.sleep()` | Enter quiescent mode (§38) |
| `display.wake()` | Resume from quiescent mode; replay last snapshot |

**Visual-only** (physical-display bridges):

| Service method | Bridge action |
|---|---|
| `display.set_brightness(level)` | Call panel driver `set_brightness`; emit watch |
| `display.set_rotation(r)` | Rebuild activities (§36); emit watch |
| `display.set_screen_timeout(d)` | Store for auto-dim policy (no bridge action) |

A headless or voice-only platform declares a reduced `system.display` capability surface in its service registry — callers of `display.set_brightness` on such a device get `:err :unavailable`, not a silent no-op.

## 44 · `system.theme`

| Service method | Bridge action |
|---|---|
| `theme.set(id)` | Rebuild activities with new palette (§37) |
| `theme.watch()` | Bridge emits on set |

`theme.current()` reads the bridge's authoritative palette reference.

## 45 · `system.notify`

| Service method | Bridge action |
|---|---|
| `notify.toast(msg, dur?)` | Present Toast Service (§25) |
| `notify.post(opts)` | Queue persistent notification (bridge maintains a history; depending on substrate, surfaces as a tray / voice prompt / terminal banner) |
| `notify.set_badge(n)` | Update app-level badge via Badge Service (§35) |

Bridge does NOT persist notifications across reboots — that's `system.notify`'s job.

## 46 · `system.security`

| Service method | Bridge action |
|---|---|
| `sec.set_pin(pin)` | No bridge action (storage handled by service) |
| `sec.verify_pin(pin)` | No bridge action (validation handled by service) |
| `display.lock()` (§43) | Bridge presents Lockscreen and calls `sec.verify_pin` on entry |

The lockscreen is the bridge's only direct coupling to `system.security`. All other security ops go through the service.

## 47 · `system.apps`

| Event / stream | Bridge action |
|---|---|
| `apps.launched (app_id)` | Push new activity; request initial snapshot |
| `apps.suspended (app_id)` | Keep activity in stack (suspend-to-home) OR tear down (pop-to-home), per memory pressure policy |
| `apps.terminated (app_id)` | Pop activity from stack; tear down widgets |
| `apps.notif_counts_watch()` | Update badges on launcher grid and on `navigate` rows |
| `apps.current_watch()` | Update statusbar app title |

The bridge does not drive the activity stack itself — `system.apps` lifecycle events do. This decouples presentation from app lifecycle.

## 48 · `system.share`

The bridge presents the Share Sheet (§32) by enumerating services that register as `share.target` consumers (SERVICES §44). The selected target receives the payload via the `share.target` IPC.

Bridge rule: the share sheet enumeration is deterministic — `share.target` candidates are listed alphabetically, with built-in targets (Copy, Save to file) appended at the bottom.

## 49 · `system.intents`

The bridge honours cross-app deep-link intents routed through `system.intents` (`@handles` in apps). When the runtime dispatches an intent to a target app, the bridge:

1. Pushes the target app (launching if cold).
2. Awaits the first non-loading snapshot.
3. Renders.

Back from the target app returns to the originator (via `@on back`).

## 50 · `system.logs`

The bridge emits its own log entries through `logs.emit` for:

- First-occurrence unsupported content kind (`:warn`).
- Performance budget exceeded (`:warn`).
- Rendering failure (`:error`).
- Theme / rotation / lock state changes (`:debug`).

The bridge does NOT read logs; that's for system diagnostic apps.

---

# Part VII — Reference bridge: CyberDeck (LVGL 8.4 on Waveshare ESP32-S3-Touch-LCD-4.3)

This part specifies the reference implementation of the bridge. Alternative bridges may diverge; the invariants in Part VIII still hold.

## 51 · Hardware substrate

| Property | Value |
|---|---|
| SoC | ESP32-S3, dual-core Xtensa LX7 @ 240 MHz |
| Display | 800 × 480 px, RGB LCD (16-bit parallel) |
| Touch | GT911 capacitive, 5-point, I2C (SDA=GPIO8, SCL=GPIO9) |
| PSRAM | 8 MB OPI |
| Flash | 8 MB |
| UI library | LVGL 8.4.0 |
| UI core | Core 1 (`lvgl_task`) |
| Runtime core | Core 0 (`deck_runtime_task`) |

Board-level notes:

- No runtime-usable hardware buttons. GPIO 0 is owned by the RGB LCD peripheral (DATA6). Navigation is touch-only.
- No Bluetooth Classic on the SoC; optional external BT module on UART1.
- BLE is native.
- CH422G expander's bit 5 (USB_SEL) must stay LOW — routes USB native to the native port (required for flash + JTAG).

## 52 · Partition and thread layout

```
┌──────────────────────┐                     ┌──────────────────────┐
│ Core 0 — Runtime     │  MPSC queue         │ Core 1 — LVGL task   │
│  deck_runtime_task   │ ─── snapshots ────> │  lvgl_task           │
│                      │                     │  (bridge.ui driver)  │
│                      │ <─── intents ─────  │                      │
└──────────────────────┘                     └──────────────────────┘
                                                        │
                                                        │ display.panel.flush
                                                        │ display.touch.read
                                                        ▼
                                             ┌──────────────────────┐
                                             │  Hardware drivers    │
                                             │  RGB LCD, GT911, …   │
                                             └──────────────────────┘
```

LVGL mutex (`deck_bridge_ui_lock`) gates any LVGL object access from non-LVGL threads. Timeout 0 = block forever; caller MUST call `deck_bridge_ui_unlock` in every path.

## 53 · LVGL integration

- Display driver registered via `lv_disp_drv_register`; framebuffer in PSRAM, double-buffered, flushed via DMA.
- Touch input device registered via `lv_indev_drv_register`; `touchpad_read` returns raw coordinates.
- `lv_tick_inc` fed by FreeRTOS tick on Core 1.
- `lv_timer_handler` runs in the LVGL task loop.

### 53.1 LVGL 8.4 gotchas

| Issue | Rule |
|---|---|
| Scrollbar styling | `lv_obj_set_style_width(obj, 2, LV_PART_SCROLLBAR)` — no `lv_obj_set_style_scrollbar_width()` in v8 |
| No layout constant | Use `0` instead of `LV_LAYOUT_NONE` |
| Stuck-pressed buttons | Always `lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE)` after `LV_OBJ_FLAG_CLICKABLE` |
| `-Werror` | All warnings fatal; watch `snprintf` truncation on small buffers |
| Touch + rotation | Never transform coordinates in `touchpad_read`; LVGL does it |
| Canvas buffer ownership | `lv_color_t*` canvas buffers are heap-owned by caller; free in dismiss/destroy |
| Screen-load ordering | `lv_scr_load(new)` MUST precede `lv_obj_del(old)` to prevent dangling `act_scr` |
| Event handler cleanup | Unregister all LVGL event handlers before freeing widgets |

## 54 · Palette (three themes)

| Field | Green (Matrix) | Amber (Retro) | Neon (Cyberpunk) |
|---|---|---|---|
| `primary` | `#00FF41` | `#FFB000` | `#FF00FF` |
| `primary_dim` | `#004D13` | `#4D3500` | `#4D004D` |
| `bg_dark` | `#000000` | `#000000` | `#000000` |
| `bg_card` | `#0A0A0A` | `#0A0A0A` | `#0A0A0A` |
| `text_dim` | primary @ `LV_OPA_60` | primary @ `LV_OPA_60` | primary @ `LV_OPA_60` |
| `accent` | `primary` | `primary` | `#FF0055` |
| `success` | `primary` | `primary` | `#39FF14` |
| `destructive_fixed` | `#FF3333` | `#FF3333` | `#FF3333` |

Destructive colour is theme-invariant (§17.6).

## 55 · Typography

Montserrat family. All content text is **ALL CAPS** except:

- Toast body (§25.3).
- Error reason strings (rendered as declared — the app wrote the sentence).
- Markdown content body (apps write natural-case markdown).
- User-entered text in textareas (mirror input).

| Alias | Font | Reference use |
|---|---|---|
| SM | Montserrat 18 | Statusbar labels, captions, field labels, list secondary, toast |
| MD | Montserrat 24 | Body, list primary, data value, button label |
| LG | Montserrat 32 | Launcher card icons in grid |
| XL | Montserrat 40 | System clock, launcher XL icon, PIN dots, loading cursor |

**Symbol rules:** reference bridge uses only `LV_SYMBOL_*` macros and custom canvas graphics for terminal-aesthetic icons (triangle BACK, circle HOME, square TASKS). Raw UTF-8 arrows / bullets render as rectangles — forbidden in bridge-authored text.

## 56 · Metrics

| Context | Value |
|---|---|
| Content area `pad_all` | 16 px |
| Flex column `pad_row` | 14 px |
| Section gap between unrelated groups | 18 px |
| Section label `pad_top` | 20 px |
| List item `pad_vertical` / `pad_horizontal` | 12 px / 8 px |
| Grid gap (launcher, button group) | 12 px |
| Card icon-to-name gap | 4 px |
| Dialog body `pad_all` / `pad_row` | 24 / 16 px |
| Dialog button gap above action row | 24 px |
| Dialog action row `pad_column` | 8 px |
| Toast `pad_horizontal` / `pad_vertical` | 12 / 6 px |
| Statusbar height | 36 px |
| Navbar thickness | 60 px |
| Dialog width | 380 px |
| Dialog title polygon height | 28 px |

| Widget | Border | Radius | Border side |
|---|---|---|---|
| Card / major container | 2 px | 12 px | all |
| Launcher app card | 2 px | 16 px | all |
| Button | 2 px | 12 px | all |
| Toast | 1 px | 2 px | all |
| Dialog | 1 px | 2 px | all |
| Text input | — | 8 px | all |
| List item row | 1 px | 0 | bottom only |
| Navbar | 2 px | 0 | top (portrait) / left (landscape) |
| Statusbar separator | 2 px | 0 | top + bottom edges |
| Scrollbar | 2 px | 0 | — |
| PIN numpad button | — | 6 px | all |
| Badge circle | — | 50 % (round) | — |

## 57 · Gesture zones (reference)

| Gesture | Motion | Portrait | Landscape |
|---|---|---|---|
| HOME | Swipe up from bottom edge | ≥60 px upward in bottom 80-px strip | ≥60 px upward in bottom 80-px strip |
| BACK | Swipe down from top edge | ≥60 px downward in top 80-px strip | ≥60 px downward in top 80-px strip |
| TASKS | Long press on navbar HOME | > 600 ms | > 600 ms |
| App settings | Long press on launcher grid cell | > 600 ms | > 600 ms |

## 58 · Performance targets (measured on reference hardware)

| Metric | Target | Current measured |
|---|---|---|
| Bridge render time per snapshot (50 nodes, rebuild) | ≤ 16 ms | ≤ 14 ms |
| Bridge render time per snapshot (50 nodes, patch) | ≤ 4 ms | ≤ 3 ms |
| Overlay present | ≤ 32 ms | ≤ 25 ms |
| Rotation rebuild | ≤ 500 ms | ≤ 280 ms |
| Theme rebuild | ≤ 500 ms | ≤ 240 ms |
| Memory (bridge heap peak) | ≤ 512 KB | ≤ 420 KB |
| Memory (per-activity overhead) | ≤ 32 KB | ≤ 22 KB |
| Cold-boot to first content frame | ≤ 2 s | ≤ 1.5 s |

---

# Part VIII — Conformance

A bridge implementation conforms to a Deck level (`:DL1` / `:DL2` / `:DL3`). The level gates what apps the platform can host. The minimum bridge for a DL level is defined by the content kinds, UI services, and inference invariants it must support.

## 59 · Per-level bridge requirements

### 59.1 DL1 — Core

Bridges are **optional at DL1**. A DL1 platform may be headless (sensor node, actuator). When present:

| Component | Required |
|---|---|
| Content kinds | `:data` (bare values), `:group`, `:list`, `:list_item`, `:trigger`, `:navigate`, `:loading` |
| UI services | Toast |
| Subsystems | — |
| Invariants | §20 `invariants across all bridges` applies |

A DL1 app cannot use `content =` inside machines (that is DL3). DL1 apps with UI use a simpler top-level content form pinned to `@on launch`.

### 59.2 DL2 — Standard

Requirements marked **(if substrate applies)** apply only to bridges whose substrate supports the concept; a voice bridge is DL2-conformant without them if it satisfies the substrate-native equivalents via its resolution services.

| Component | Required | Notes |
|---|---|---|
| Content kinds | everything in DL1 **plus** `:form`, `:error`, `:status`, `:rich_text`, `:progress`, `:media`, `:toggle`, `:range`, `:choice`, `:text`, `:password`, `:search`, `:date`, `:confirm`, `:create`, `:share` | Universal — every bridge decodes these |
| Resolution services | Toast, Confirm, Loading, Progress, Keyboard, Choice, Date, Share, Permission, Lockscreen | Universal — substrate decides the rendering (visual overlay / voice prompt / readline) |
| Visual ambient indicators | Statusbar (minimal), Navbar, Badge | **(if substrate applies)** — spatial substrates only |
| Physical-display subsystems | Rotation, Brightness, Gesture (HOME/BACK) | **(if substrate applies)** — physical-display + touch substrates only |
| Theme | At least one theme (palette or voice profile) | Universal — substrate-specific expression |
| Invariants | §20 + intent lossless + modal-context exclusivity + `@on back :confirm` | Universal |

### 59.3 DL3 — Full

| Component | Required | Notes |
|---|---|---|
| Content kinds | everything in DL2 **plus** `:multiselect`, `:pin`, `:markdown`, `:markdown_editor`, `:chart` (placeholder allowed), `:list_empty`, `has_more`/`on more` | Universal |
| Resolution services | All DL2 + Progress cancellable | Universal |
| Visual ambient indicators | Full Statusbar (BT/SD/audio), full Navbar (HOME+BACK+TASK-SWITCH) | **(if substrate applies)** |
| Physical-display subsystems | Rotation (2+ orientations), Brightness, Gesture zones (HOME + BACK + TASKS + App-settings) | **(if substrate applies)** |
| Theme | 3 palettes / voice profiles | Universal |
| Rebuild semantics | Rebuild-on-theme; rebuild-on-substrate-context-change (rotation if visual, voice profile change if voice) | Universal |
| Assets via AssetRef | full | Universal |
| Invariants | §20 + `@on back :confirm` | Universal |

CyberDeck (reference) claims DL3 with the physical-display + touch substrates fully populated.

## 60 · Conformance test suite

A conformance test bundle exercises the bridge through the SDI without any app running:

| Test | Verifies |
|---|---|
| `bridge.content.kinds` | Every required content kind renders without crash |
| `bridge.overlay.exclusivity` | Opening a second overlay dismisses the first |
| `bridge.intent.roundtrip` | Every tap fires exactly one intent with correct payload |
| `bridge.diff.patch` | Same-state leaf update does not teardown the subtree |
| `bridge.diff.rebuild` | Shape change rebuilds cleanly |
| `bridge.rotation` | Rotation preserves identity stack and re-presents |
| `bridge.theme` | Theme swap rebuilds without leaks |
| `bridge.lockscreen` | Lock + PIN verify + unlock flow |
| `bridge.back.confirm` | `@on back :confirm` displays supplied labels |
| `bridge.back.handled` | `@on back :handled` consumes gesture |
| `bridge.memory.leak` | 100 push_snapshot / rotate cycles; bridge heap returns to baseline |
| `bridge.perf.budget` | Performance targets met within ±20 % |

Conformance is measured at boot by the platform's `deck_conformance` component and reported via `system.platform`.

---

# Part IX — Authoring alternative bridges

The spec is deliberately substrate-independent. A new bridge implementation picks a substrate, a set of UI services, and a conformance profile.

## 61 · Substrate matrix

| Substrate | Content rendering | Overlay strategy | Input |
|---|---|---|---|
| `:framebuffer` | Pixels (reference) | Modal layer-top | Touch / keyboard |
| `:eink` | Bitmap, limited refresh | Page replacement | Touch / buttons |
| `:voice` | TTS | One prompt per turn | ASR commands |
| `:terminal` | Character grid + ANSI | Status line / modal | Keyboard |
| `:headless` | None | None | None |

## 62 · Minimum viable bridge — voice substrate

A voice-only bridge implements:

- Content decoder: reads `data`, `status`, `rich_text`, `list` (enumerate with indices), `group` (label + children), `form` (ask inputs one-by-one).
- Intents: reads input prompts, confirms with `"say yes / say cancel"`, maps ASR → intent fire.
- UI services: Toast → short pre-prompt announcement; Confirm → "say yes or no"; Loading → silent + timeout; Choice → "options are 1, 2, 3 — say a number"; Permission → "allow X? yes or no"; Lockscreen → spoken PIN entry digit by digit.
- Subsystems: no rotation, no theme, brightness semantic-only, gesture via hotwords.

Apps built for the reference touchscreen bridge run on this voice bridge **unchanged**. The same Bluesky content tree produces:

- Touchscreen: scrollable timeline, tap-to-like, swipe-to-compose.
- Voice: "Timeline: one, Ada posted 2 minutes ago, 'Hello world'. Say 'next' for more, 'like' to like, 'open' to view thread."

This is the validation test: a bridge is correct iff any well-formed Deck app runs against it without modification.

## 63 · Compliance check for alternative bridges

Before shipping a new bridge:

1. Run the conformance suite (§60) — all tests must pass.
2. Run the reference app catalog (Launcher, Task Manager, Settings, Files, Bluesky) — all flows must complete without the app being aware of which bridge renders them.
3. Verify invariants (§20) hold by code review.
4. Declare `deck_level:` in the platform manifest; reject any app whose `@requires.deck_level` exceeds the bridge's profile.

---

# Part X — Changes from earlier drafts

### X.1 Removed
- `bridge.ui.*` as a Deck capability. Apps cannot `@use` the bridge. Removed from `@needs.caps` / `@grants.services`. Any app attempting to import `bridge.ui` → `LoadError :unresolved`.
- `ui_common_column / row / card / grid / data_row / action_row / nav_row` as app-facing vocabulary. These were legacy C helpers; the equivalent bridge patterns are **inferred** from semantic primitives (§17).
- `shell.set_statusbar(bool)` / `shell.set_navigation_bar(bool)` methods. Statusbar and navbar are rendered unconditionally; apps cannot toggle them. Future full-screen apps declare `@app.fullscreen: true` as an identity field.
- `status_bar` / `nav_bar` content primitives. Apps never author these.
- `@app.icon:` as bridge-inference input remains, but `icon:` / `badge:` fields on `trigger` / `navigate` are data, not content primitives.

### X.2 Added
- Part III (Inference rules) as a first-class section. The reference policy is now a documented artefact, not C code that readers must reverse-engineer.
- Gestalt as universal design frame (§11); 4-axis rubric for spatial substrates (§12); 4-axis rubric for voice substrates (§12bis — Duration / Order / Pause / Repetition analogues of Size / Position / Space / Alignment).
- `@on back :confirm` bridge contract (§24.3). The runtime delivers the structured confirm request to the bridge, which renders it using the same Confirm Dialog Service.
- Substrate matrix (§61) and compliance check (§63). The bridge spec is now explicitly device-independent.
- SDI vtable stratified by substrate scope (§4) — **Core** / **Visual** / **Visual-input** / **Physical-display** bands with a stubbing convention (`DECK_SDI_ERR_NOT_SUPPORTED` for out-of-substrate methods).
- Modal context (§22) as the universal concept behind overlays, dialogs, and lockscreen — with spatial, voice, and terminal renderings.
- Per-service substrate-rendering sub-sections in Part IV (Toast, Loading, Progress, Choice, Date, Lockscreen) documenting spatial / voice / terminal presentations of the same semantic contract.
- Scope markers (`Substrate scope:` italic prefix) on every substrate-specific section so readers can trace universal-vs-reference at a glance.

### X.3 Simplifications
- Six content primitives (`:group`, `:list`, `:form`, `:loading`, `:error`, `:*_data_wrapper`) + 14 intents covers the full surface. No layout primitives remain.
- No animation knobs. Transition effects are bridge-owned.
- No customisable themes. Three reference themes; apps never specify colour.
- Single snapshot format per bridge. No per-screen opt-ins. Presentation context (orientation, density, viewport geometry) is bridge-internal — never on the wire.

### X.4 Kept
- `10-deck-bridge-ui.md` §4 DVC catalog (refined wire format remains in `11-deck-implementation §18`).
- §5 UI services catalog (entries migrated to Part IV with consistent spec structs).
- §7 inference rules (migrated to Part III with explicit Gestalt framing).
- §11 LVGL gotchas (migrated to §53.1).
- All reference metrics and palette (migrated to Part VII).

---

# Part XI — Open questions

1. **Animation vocabulary.** Should the bridge define a small set of animation atoms (`:push`, `:replace`, `:fade`, `:none`) that apps declare on state transitions, or should transitions remain mute? Current spec: mute. Rationale: apps don't know what "push" looks like on voice.
2. **Per-app theme override.** Should apps be able to request a specific theme for their content (e.g., a camera app forcing dark)? Current spec: no. Rationale: user owns device-wide theme.
3. **Multi-overlay queueing.** Current policy: new overlay replaces old. Alternative: queue. Likely confusing; keep replacement.
4. **Embedded media beyond images.** Video, inline audio player, interactive canvases. Not in core spec; bridge policy may extend via `:media` roles.
5. **Accessibility-first bridge.** A bridge variant where every content primitive is annotated with a11y role. Voice-assistive, screen-reader-compatible touchscreen. Belongs in a future DL3 extension.
6. **Typography system beyond ALL-CAPS.** Apps may need case-sensitive rendering (posts, messages, code). Markdown and `rich_text` already do this; extend the policy so that bare `str` expressions rendered as content also respect case? Current reference: ALL CAPS for labels/data, natural case for longform. This needs explicit formalisation.
7. **`share` target selection policy.** Deterministic order currently; should the bridge rank by last-used, by frequency? Needs UX research.
8. **Voice-substrate 4-axis rubric (§12bis) validation.** The rubric was specified but no voice bridge has been implemented against it; the real test is whether Bluesky's kitchen-sink flow comes out legible through a voice bridge. Likely refinement ahead.
9. **Mixed-substrate bridges.** Could a single bridge address two substrates at once (e.g., framebuffer + voice co-operating)? Current spec assumes one bridge per platform with one primary substrate. A multi-substrate bridge would need explicit policy on which substrate owns a given interaction. Out of scope for v1.
10. **Screen-reader accessibility as its own substrate.** Related to #5 but distinct: a "sighted + screen-reader" dual-output mode where both a visual surface and a voice stream present the same content with appropriate detail levels. Probably a later substrate atom (`:accessible`) rather than a flag on `:framebuffer`.
11. **Coalescing of rapid-fire dependency changes.** A stream emitting at high frequency (`@on every 10ms`) with a content body that reads `N.last()` re-evaluates 100× / s. The runtime is expected to coalesce dependency invalidations within a single scheduler tick; not formalised yet. Reference bridge will clamp render rate to substrate refresh (≈ 60 Hz on framebuffer); the runtime should avoid producing snapshots faster than the bridge consumes them. Needs a bounded-backlog rule in `DRAFT.md §22` or equivalent.
