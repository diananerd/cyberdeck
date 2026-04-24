# Fixture Audit — `apps/conformance/`

Read-only triage of every `.deck` file under `apps/conformance/` against the five-pillar spec in `deck-lang/` (LANG / SERVICES / CAPABILITIES / BUILTINS / BRIDGE).

**Global finding.** Every single fixture uses `@requires` — the old name for `@needs` (LANG §8). Every fixture therefore needs at minimum the mechanical `@requires → @needs` rename. That rename is treated as the baseline "YELLOW" condition throughout the table; fixtures listed as GREEN are GREEN *modulo the global `@requires → @needs` rename*, which is a file-header mechanical change with no semantic impact. If you prefer to count any `@requires` use as YELLOW, every row below ticks up by one.

For the table below, "GREEN" = only the global `@requires → @needs` rename is needed; "YELLOW" = additional mechanical renames needed; "RED" = semantic reshape; "DROP" = feature gone, no replacement.

---

## Triage table

| Fixture | Bucket | Old → New | Notes |
|---|---|---|---|
| `app_assets.deck` | YELLOW | `&&` → `and` | Uses `&&` in boolean join. `@assets` + `asset.path` + `:some`/`:none` patterns are spec-canonical. |
| `app_bridge_ui.deck` | RED | `label`, `divider` → semantic primitives; bare `transition :atom` → `on :event from :src to :dst` | Uses two non-primitives (`label "…"`, `divider`) not in LANG §15 / BRIDGE §13; uses the old bare `transition :done` auto-transition form that isn't in LANG §13.3. Rewrite to bare-expression data wrappers + explicit `on :go from :init to :done` and `@on launch` pushing the event. |
| `app_flow.deck` | RED | `@flow` + `step` → `@machine` + states with explicit transitions | LANG §13 has `@machine` only; there is no `@flow` or `step`. Rewrite as a 3-state machine with `on :next from :s to :d` transitions driven by `on enter` → `<Machine>.send(:next)`. |
| `app_machine.deck` | RED | bare `transition :ready` → `on :ready from :boot to :ready` | LANG §13.3 requires `on :event from :src to :dst`; no bare auto-transition shorthand exists. Initial-state auto-advance must be modelled as `state boot on enter -> self.send(:go)` + an explicit `on :go` transition. |
| `app_machine_hooks.deck` | RED | bare `transition :ready`; `@machine.before` / `@machine.after` hooks | Same `transition :atom` issue as above. `@machine.before` / `@machine.after` are not in LANG §13 — per §13.5 only `state :s on enter` / `on leave` exist. Rewrite the hook intent inside `on leave` / `on enter`. |
| `edge_comments.deck` | YELLOW | `#` comments → `--` | LANG §1.2: only `--`. Seven `#` comment lines to convert. |
| `edge_deep_let.deck` | GREEN | | Pure `let` chain + bool join; spec-canonical. |
| `edge_double_neg.deck` | GREEN | | `not not v` is spec-canonical (LANG §5.1). |
| `edge_empty_strings.deck` | GREEN | | Uses `text.len`, `text.upper`, `++`, `and` — all canonical. |
| `edge_escapes.deck` | GREEN | | String escapes + `text.len` — canonical. |
| `edge_float_special.deck` | GREEN | | `and` used throughout. |
| `edge_int_limits.deck` | GREEN | | `and` join. |
| `edge_long_ident.deck` | GREEN | | |
| `edge_long_string.deck` | GREEN | | `text.starts` / `text.ends` / `text.contains` / `++` canonical. |
| `edge_match_deep.deck` | GREEN | | Canonical `\| pat -> expr` arms. |
| `edge_match_when.deck` | GREEN | | Canonical `when` guard syntax (LANG §5.6). |
| `edge_nested_let.deck` | GREEN | | |
| `edge_nested_match.deck` | GREEN | | Nested `match` canonical. |
| `edge_string_intern.deck` | GREEN | | |
| `edge_unicode.deck` | YELLOW | `#` comment → `--` | One `#` line; otherwise canonical. |
| `err_capability_missing.deck` | GREEN | | Unknown `unknowncap.foo` call — the probe's intent (load-error on unresolved capability) still valid. |
| `err_deck_os.deck` | GREEN | | `deck_os: 99` triggers `LoadError :incompatible` per LANG §8. |
| `err_div_zero_float.deck` | GREEN | | Division-by-zero panic probe. |
| `err_div_zero_int.deck` | GREEN | | |
| `err_edition.deck` | GREEN | | `edition: 9999` incompatible — still spec-valid probe. |
| `err_effect_undeclared.deck` | YELLOW | `!http` effect alias → bare `!` | LANG §2.6: purity is a single bit (`!`). Named effect aliases after `!` were removed. Fn signature should be `fn fetch (url) ! = url`. The "undeclared effect" probe semantics must be rethought — there's nothing to declare — so this may also be a RED depending on what the harness expects; flagging YELLOW because the mechanical rewrite is unambiguous and the resulting fixture still exercises an impure fn with no capability, which the loader will reject on a different axis (unused `!`). |
| `err_fn_arity.deck` | GREEN | | `add(2)` should fail arity check. |
| `err_level_high.deck` | GREEN | | `deck_level: 3` on a DL1 runtime. |
| `err_level_unknown.deck` | GREEN | | `deck_level: 99` unknown. |
| `err_match_noexh.deck` | GREEN | | Non-exhaustive match probe. |
| `err_missing_id.deck` | GREEN | | `@app` without `id:` is still a LoadError. |
| `err_mod_zero_int.deck` | GREEN | | |
| `err_parse_error.deck` | GREEN | | Dangling `1 +` — parse error probe. |
| `err_required_cap_unknown.deck` | YELLOW | `capabilities:` → `caps:` | LANG §8 uses `caps:` / `services:`, not `capabilities:`. |
| `err_str_minus_int.deck` | GREEN | | Type-error probe. |
| `err_type_mismatch.deck` | GREEN | | |
| `err_unresolved_symbol.deck` | RED | bare `transition :ghost` → explicit `on :event from :src to :dst`; unresolved target state still the intended probe | Same machine-syntax issue as `app_machine.deck`; the probe's intent (unresolved state atom `:ghost`) is still valid, just needs reshape. |
| `lang_and_or_kw.deck` | GREEN | | Canonical `and` / `or` / `not`. |
| `lang_arith.deck` | YELLOW | `&&` → `and` | Uses `&&` in bool joins throughout. Arithmetic itself is canonical. |
| `lang_compare.deck` | YELLOW | `&&` → `and` | Same as above. |
| `lang_fn_basic.deck` | YELLOW | `do` block → implicit sequencing | LANG §5.4: no `do` keyword — bodies are implicit blocks. One `fn pipeline` uses `do`; remove the keyword. |
| `lang_fn_block.deck` | GREEN | | Already uses implicit sequencing; no `do`, no `&&`. |
| `lang_fn_mutual.deck` | GREEN | | |
| `lang_fn_recursion.deck` | GREEN | | |
| `lang_fn_typed.deck` | YELLOW | `do` block → implicit sequencing | One `fn no_ret` uses `do`; drop the keyword and the indented `log.info` becomes the body directly. `Result int str` return type is canonical (LANG §2.4). |
| `lang_if.deck` | GREEN | | `if / then / else` is canonical sugar (LANG §5.5). |
| `lang_interp_basic.deck` | GREEN | | `{expr}` interpolation canonical (LANG §1.6). |
| `lang_lambda_anon.deck` | GREEN | | `fn (…) = …` anon form + `x -> …` arrow form — both canonical (LANG §4.4). |
| `lang_lambda_basic.deck` | YELLOW | `&&` → `and` | Single bool join; otherwise canonical. |
| `lang_lambda_closure.deck` | YELLOW | `&&` → `and` | |
| `lang_lambda_higher_order.deck` | YELLOW | `&&` → `and` | |
| `lang_lambda_inline.deck` | GREEN | | Uses `and` throughout. |
| `lang_let.deck` | GREEN | | Canonical `let` + annotations. |
| `lang_list_basic.deck` | GREEN | | Canonical `list.*` + `\| :some x -> …` match arms. |
| `lang_literals.deck` | YELLOW | `&&` → `and` | Uses `&&` heavily; note fixture already deliberately avoids range-as-iterable (§2.9). |
| `lang_logic.deck` | GREEN | | Written specifically to exercise `and`/`or`/`not`. |
| `lang_map_basic.deck` | GREEN | | |
| `lang_match.deck` | GREEN | | |
| `lang_metadata.deck` | YELLOW | `@permissions` → `@grants`; `optional` placement under `@use` | LANG §10: annotation is `@grants`, not `@permissions`. LANG §9: `crypto.aes as aes optional` is a YELLOW — spec puts `optional` under `@needs.caps` (§8), not after the `@use` alias. `@errors <domain>`, `@private`, `fn helper` all canonical. |
| `lang_pipe_is.deck` | RED | `is` operator → `== :atom` + `type_of(v) == :Name`; `\|>?` → postfix `?` | LANG §5.7 removes `is`; LANG §5.3 removes `\|>?`. The whole fixture's probe shape (pipe + `is`) must be rewritten. Intent still valid — split into separate pipe and type-test probes. |
| `lang_requires_caps.deck` | YELLOW | `capabilities:` → `caps:` | Same `capabilities` → `caps` rename as `err_required_cap_unknown.deck`. |
| `lang_stdlib_basic.deck` | GREEN | | Docstring already notes prior `!is_int` / `ok()` issues were fixed to `not` / `:ok v`. |
| `lang_strings.deck` | GREEN | | `++` concat canonical (LANG §5.2). |
| `lang_tco_deep.deck` | GREEN | | Pure recursion. |
| `lang_tuple_basic.deck` | GREEN | | |
| `lang_type_record.deck` | GREEN | | Canonical `@type` + `TypeName { … }` construction + `with` would also be canonical. |
| `lang_utility.deck` | GREEN | | Canonical `time.now` + `time.since`. |
| `lang_variant_pat.deck` | GREEN | | |
| `lang_where.deck` | RED | `where` block → chained `let` | LANG §3.1: no `where` keyword. Every `where a = …, b = …` has to become leading `let` lines before the expression that used to close with `where`. `match` arms that used the trailing `where` need the bindings hoisted to the top of the arm body. Intent (scoped helpers) remains valid. |
| `lang_with_update.deck` | GREEN | | Docstring already notes prior non-spec forms were fixed. |
| `os_conv.deck` | YELLOW | `&&` → `and` | Conversion builtins canonical. |
| `os_fs.deck` | YELLOW | `&&` → `and` (bool joins); `let _ = expr` discard idiom | `fs.*` calls canonical per BUILTINS / SERVICES. The `let _ = …` to discard impure return is legal but may want review against LANG §3 wildcard rules. |
| `os_fs_list.deck` | YELLOW | `&&` → `and` | `fs.list` Result destructure canonical. |
| `os_info.deck` | YELLOW | `&&` → `and` | `system.info.*` accessors canonical. |
| `os_lifecycle.deck` | GREEN | | `@on launch` / `resume` / `suspend` / `terminate` / `back` canonical (LANG §14); `@on back` returns `:handled` which matches LANG §14.8. |
| `os_math.deck` | YELLOW | `&&` → `and` | Assumes `math.abs_int`, `math.clamp_int`, `math.gcd`, `math.lcm` etc. exist in BUILTINS — verify against that spec, but syntactically it's YELLOW only for the bool joins. |
| `os_nvs.deck` | YELLOW | `&&` → `and` | `nvs.*` canonical. |
| `os_text.deck` | YELLOW | `&&` → `and` | `text.*` canonical; fixture deliberately uses `text.len` / `text.starts` / `text.ends` spellings that may disagree with BUILTINS — flag as YELLOW only; the naming drift is the fixture's point. |
| `os_time.deck` | YELLOW | `&&` → `and` | `time.*` + Duration arithmetic canonical. |
| `sanity.deck` | GREEN | | Minimal app; only uses the global `@requires → @needs` rename. |

---

## Summary

### Totals (80 fixtures)

| Bucket | Count |
|---|---|
| GREEN (only global `@requires` → `@needs`) | 49 |
| YELLOW (mechanical renames beyond the global one) | 23 |
| RED (semantic reshape) | 7 |
| DROP | 0 |

### Top 5 most-impactful changes

1. **`@requires` → `@needs`** — 80/80 fixtures. The whole conformance suite uses the old annotation name. This is a project-wide find-replace at the first annotation header.
2. **`&&` / `||` → `and` / `or`** — 16 fixtures. LANG §5.1 defines only the keyword operators; C-style operators are not in the grammar. Mechanical swap, but repeated across every row-joining expression.
3. **`transition :atom` shorthand inside `state` → explicit `on :event from :src to :dst`** — 4 fixtures (`app_bridge_ui`, `app_machine`, `app_machine_hooks`, `err_unresolved_symbol`). LANG §13.3 has no implicit / auto-transition shorthand; every transition needs an event atom and explicit from/to. Paired with this, `@machine.before` / `@machine.after` (one fixture) must be re-expressed as `state on enter` / `on leave` hooks.
4. **`capabilities:` → `caps:`** — 2 fixtures, plus the global understanding that `@needs.caps:` / `@needs.services:` (LANG §8) is the canonical keying.
5. **`#` comments → `--` comments** — 2 fixtures (`edge_comments`, `edge_unicode`). LANG §1.2: only `--`.

Runners-up (all YELLOW): `do` keyword removal (3 fixtures — LANG §5.4), `@permissions` → `@grants` (1 fixture — LANG §10), and the sprinkling of `where` clauses (1 fixture — LANG §3.1).

### Fixtures recommended to DROP

**None.** Every RED fixture tests an intent that is still valid under the new spec — the probe needs to be rewritten, not discarded:

- `app_bridge_ui` still exercises content-body semantic primitives; just needs the two legacy primitives swapped out.
- `app_flow`, `app_machine`, `app_machine_hooks`, `err_unresolved_symbol` all test machine-lifecycle semantics; the same semantics are expressible under LANG §13.3 with explicit transitions.
- `lang_pipe_is` tests both the pipe operator (still present — LANG §5.3) and the type-test form (now two builtins — LANG §5.7). Split into two probes.
- `lang_where` tests scoped helper bindings; chained `let` (LANG §3.1) covers the same ground.

One *candidate* for DROP is `lang_pipe_is` if splitting-into-two feels like duplication with `lang_stdlib_basic` (which already covers Optional / Result helpers), but keeping the rewritten version is cheap and exercises the pipe operator specifically, which otherwise has no dedicated fixture.

### Notes for the rewrite pass

- Several YELLOW fixtures (`os_text`, `os_math`, `os_nvs`, `os_fs`, `os_fs_list`, `os_info`, `os_time`, `os_conv`) claim coverage of BUILTINS / SERVICES surfaces using specific method names (`text.starts`, `math.abs_int`, `nvs.get_int`, `fs.list` shape, `system.info.device_id()`, etc.). Those names were not re-verified against BUILTINS.md / SERVICES.md during this audit — confirm during the rewrite pass, since a BUILTINS-level rename would bump these from YELLOW to RED.
- `err_effect_undeclared` is the most semantically awkward fixture: the whole concept of "undeclared effect" is less meaningful now that `!` is just a single bit. Consider reframing it as "impure fn called from pure context" (still a LoadError) rather than keeping the effect-name probe.
- The global `@requires → @needs` rename is the right first pass — once every fixture parses against the new header grammar, the remaining YELLOW/RED classifications can be done with the loader actually running the files.
