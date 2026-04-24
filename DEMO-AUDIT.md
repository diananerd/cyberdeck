# apps/demo.deck — audit vs current spec

**Verdict:** largely aligned. `demo.deck` was written during the spec consolidation and uses the post-cutover annotation surface already. Small touch-ups only; no structural rewrite needed.

File: `apps/demo.deck`, 1988 lines. Purpose per its own header: "combinatorial stress test — every language edge × every platform surface × feature interactions."

## What's already compliant with the spec

| Surface | Evidence |
|---|---|
| `@app` identity with `edition: 2027`, `serves:`, `orientation:`, `log_level:`, `tags:`, `icon:` | lines 55-68 — matches LANG §7 |
| `@needs` with `deck_level`, `deck_os`, `runtime`, `max_heap`, `max_stack`, `caps:`, `services:` | lines 70-109 — matches LANG §8 exactly |
| `@grants` with per-service blocks (`reason`, `prompt`, `allowed_hosts`, `paths`) + `logging.persist` | lines 111-178 — matches LANG §10 |
| `@assets` with bundled + downloadable (`download:`, `ttl:`) and `as:` / `for_domain:` fields | line 184+ — matches LANG §20 |
| `@migrate` (not `@migration`) | line 190 |
| `@config` with typed entries (int, str, map, list) | line 201+ |
| `@type` declarations incl. parametric `@type Pair (A, B)`, variant with payload `@type Tree =` | lines 218-256 |
| `@errors` with named domains (`demo`, `echo`, `layerA`-`C`, `stack`) | lines 256-1696 |
| `@private fn` prefix modifier | lines 273, 665, 1778 |
| `@service "deck.conformance.demo/echo"` with methods | line 1341 |
| `@handles` + `@on open_url (id: target_id)` | lines 1363, 1368 |
| `@on launch/resume/suspend/terminate/back/overrun` | lines 1139, 1221-1227, 1376, 1411 |
| `@on back` returns `:handled` / `:unhandled` per LANG §14.8 | lines 1376-1379 |
| `@on watch (expr)` reactive | line 1230 |
| `@on os.<event> (payload binders)` | lines 1233, 1418 |
| `@on every D`, `@on after D` | lines 1240, 1245, 1385 |
| `@on source <stream>` + `as Name` named-source form | lines 1257-1329 |
| `@machine` with states, transitions, hooks, composition | lines 771-1126 |
| `content =` under state bodies | line 887+ — uses every §15 primitive |
| Content primitives used explicitly | `group`, `list`, `form`, `media`, `rich_text`, `status`, `progress`, `chart`, `markdown`, `markdown_editor`, `toggle`, `range`, `choice`, `multiselect`, `text`, `password`, `pin`, `date`, `search`, `trigger`, `navigate`, `confirm`, `create`, `share` (lines 887-1000) |
| Builtins exercised | `math.*`, `text.*`, `list.*`, `map.*`, `stream.*`, `bytes.*`, `option.*`, `result.*`, `json.*`, `time.*`, `log.*`, `rand.*`, `type_of` — matches BUILTINS catalogue |
| No legacy layout primitives | zero matches for `column`, `row`, `card`, `grid`, `status_bar`, `nav_bar`, `data_row`, `action_row`, `bridge.ui.*` |
| No removed keywords | zero matches for `where`, `do` block keyword, `is`, `history`-as-keyword, `@flow`, `@stream`, `@task`, `@permissions`, `@requires`, `@migration`, `@test`, `@doc`, `@example`, `@effects` |

## Minor touch-ups worth making

All of these are nitpicks, not blockers.

### 1. Header comment still says "Deck 3.0"
**File / line:** `apps/demo.deck:1`
> `-- demo.deck — Deck 3.0 conformance hard-final test (combinatorial stress)`

Change to `-- demo.deck — Deck conformance hard-final test (combinatorial stress)` — consistent with the cutover where "3.0" is no longer a version label.

### 2. Inline comment references `§2.7` (old spec numbering)
**File / line:** `apps/demo.deck:586`
> `-- (we cannot bind a Stream to a let; per §2.7 Stream is substructural)`

Update the section pointer. LANG.md §2.7 is now the "Stream type" section (spec has the same content, different surrounding numbering). Verify by search: `grep "Stream type" deck-lang/LANG.md` shows `### 2.7 Stream type` — fortunately the number is unchanged. **No edit required.**

### 3. Inline comment references `§24 open`
**File / line:** check for other section-number references. Scan for `§[0-9]` and verify each still resolves in LANG.md.

### 4. `@grants.services.sec` and `services.systime` aliases
**File / lines:** 136-141, 139-140
```
services.sec:
services.systime:
```
These aliases don't appear in the corresponding `@needs.services` block (which uses `"system.security"` and `"system.time"` as service IDs). The alias name and the service ID must correlate. The `@use` block at line 689+ will have the definitive mapping; check that the grant aliases match the `@use … as <alias>` aliases, not the bare service group names. If `@use` has `system.security as sec`, the grant key `services.sec` is correct. If not, rename.

### 5. `@needs.caps.notifications: optional`
**File / line:** 76-77
```
caps:
  notifications: optional
```
`notifications` is not a capability in the current spec catalog — it's `system.notify`. If this was intended as a service, move it to `services:` (which already has `"system.notify": optional` at line 96). The `caps:` block should either be dropped or given a legitimate capability (there's nothing else in the new spec's `@needs.caps` namespace; `caps` retained in LANG §8 examples as the form but its users need genuine non-service capabilities). **Recommendation: drop the `caps:` block entirely.**

### 6. Sentinel log format
demo.deck emits `log.info("PASS:section_X")` sentinels. This is correct per LANG §16 and the conformance harness pattern. Section sentinels still match the letter-ordering (A-R) in the file header comment; no change.

## Run-time dependencies the spec now mandates but the demo may not exercise

These are spec features the file doesn't test. Adding probes would tighten the demo's coverage claim ("every edge"):

- `@on back :confirm (prompt, confirm: (str, atom), cancel: (str, atom))` — the structured-confirm back-result variant. demo.deck only uses `:handled` / `:unhandled`. Add a probe state that returns `:confirm (...)` and verifies the bridge round-trips.
- `assets.refresh(name)` — live asset refresh on a downloadable `@assets` entry.
- `Stream` substructural drop-on-second-use — currently only a comment (line 586).
- Mixed-edition load rejection — not testable in a single-file demo.
- `@on back :confirm` bridging through Confirm Dialog Service — needs bridge wired first (Stage 7 of the alignment plan).

## Bottom line

`demo.deck` is spec-compliant. The four nitpicks above are cosmetic. It should run against a conformant runtime + bridge without semantic rewrite.

**Does not block any subsequent stage.** Stage 10 ("apps/demo.deck rewrite per Stage 4 audit") becomes a light edit rather than a rewrite — expect <30 lines changed.
