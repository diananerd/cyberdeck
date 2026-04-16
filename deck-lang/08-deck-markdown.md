# Annex C — Markdown Expansion Module
**First-Class Markdown Rendering and Editing for Deck**

---

## 1. Design Goals

Markdown is not an afterthought in Deck — when installed, it is structurally identical to any other OS capability. It follows every pattern defined in `06-deck-native`: registered via the bridge, declared in `.deck-os`, composed into the view DSL as a native component type, accessible as builtins for pure transformation, and available as a capability for stateful operations.

**What "first-class citizen" means here:**

- `markdown` is a view component, not a workaround. It belongs in the same body DSL as `text`, `image`, and `list`. The OS renders it natively — no WebView, no HTML injection.
- Pure operations (parse, excerpt, word count, reading time, to-plain) are builtins in scope everywhere without `@use`.
- Stateful operations (editor, streaming render) are a capability declared in `@use`.
- The parsed document is a `@type MdDocument` — a real, inspectable, pattern-matchable value. Deck code can walk the AST, extract headings for a table of contents, or rewrite nodes programmatically.
- Streaming render works natively with `@stream` and reactive content bodies — appending tokens as they arrive without full re-parse.
- A `markdown_editor` component provides a native Markdown editing surface with optional toolbar and live preview.

**What this module does NOT do:**

- It does not embed a browser engine. Rendering is native UI primitives.
- It does not execute HTML embedded in Markdown (sanitized out by the parser, configurable).
- It does not implement every extension (GitHub flavored tables and task lists are included; arbitrary HTML attributes are not).

---

## 2. .deck-os Surface Definition

The `@include` directive is defined in `03-deck-os §2.1`. It inserts all declarations from the referenced file as if they were written inline. Path is relative to the including file.

```
-- markdown.deck-os
-- Include this file from your main .deck-os via:
--   @include "markdown.deck-os"

-- ── Types ────────────────────────────────────────────────────────────────────

@type MdDocument
  source     : str
  nodes      : [MdNode]
  toc        : [MdHeading]
  word_count : int
  image_urls : [str]

@type MdNode
  type     : atom
  -- :heading | :paragraph | :code_block | :blockquote | :list | :list_item
  -- :hr | :image | :table | :table_row | :table_cell | :html_block
  -- :inline_code | :bold | :italic | :strikethrough | :link | :text
  level    : int?          -- :heading only: 1-6
  text     : str?          -- for text-bearing nodes: raw text content
  lang     : str?          -- :code_block only
  url      : str?          -- :link, :image
  alt      : str?          -- :image
  title    : str?          -- :link title attribute
  ordered  : bool?         -- :list: true = ordered (1. 2. 3.), false = bullet
  tight    : bool?         -- :list: true = items are tight (no blank lines between)
  checked  : bool?         -- :list_item with GFM task syntax: - [ ] or - [x]
  align    : atom?         -- :table_cell: :left | :center | :right | :none
  is_header: bool?         -- :table_cell: true if in header row
  children : [MdNode]

@type MdHeading
  level : int
  text  : str
  id    : str    -- URL-safe slug, e.g. "getting-started"
  node  : MdNode

@type MdRange
  start : int    -- byte offset in source string
  end   : int

@type MdInlineFormat
  type  : atom   -- :bold | :italic | :code | :strikethrough | :link
  range : MdRange
  url   : str?   -- for :link

@type MdPatch
  -- Incremental update from streaming render
  type     : atom     -- :append | :replace | :finalize
  text     : str?     -- new text appended (for :append)
  document : MdDocument?  -- final full parse (for :finalize)

@type MdTheme
  -- All fields optional; unset fields inherit from OS default theme
  body_font     : str?
  code_font     : str?
  heading_scale : float?   -- multiplier on base font size; default 1.0
  line_height   : float?   -- default 1.6
  code_bg       : str?     -- CSS-like color or :surface | :muted
  link_color    : str?
  blockquote_bar: str?
  max_width     : int?     -- px; 0 = no limit

@type MdEditorState
  content     : str
  cursor      : int        -- byte offset
  selection   : MdRange?
  history_len : int        -- number of undoable actions
  active_formats: [atom]   -- formats active at cursor: [:bold, :italic, ...]

-- ── Builtins (always in scope, no @use) ──────────────────────────────────────

@builtin md
  parse         (src: str)                      -> MdDocument
  parse         (src: str, opts: {str: any})    -> MdDocument
  to_plain      (src: str)                      -> str
  to_html       (src: str)                      -> str
  excerpt       (src: str, max_chars: int)      -> str
  excerpt       (src: str, max_chars: int, suffix: str) -> str
  word_count    (src: str)                      -> int
  reading_time  (src: str)                      -> Duration
  reading_time  (src: str, wpm: int)            -> Duration
  headings      (src: str)                      -> [MdHeading]
  headings      (doc: MdDocument)               -> [MdHeading]
  heading_id    (text: str)                     -> str
  strip_images  (src: str)                      -> str
  extract_links (src: str)                      -> [(text: str, url: str)]
  extract_code  (src: str)                      -> [(lang: str, code: str)]
  has_front_matter(src: str)                    -> bool
  front_matter  (src: str)                      -> {str: any}
  body_after_front_matter(src: str)             -> str
  sanitize      (src: str)                      -> str
  sanitize      (src: str, allow_html: bool)    -> str
  node_text     (node: MdNode)                  -> str   -- recursive plain text
  node_children (node: MdNode, type: atom)      -> [MdNode]
  toc_markdown  (doc: MdDocument)               -> str   -- render ToC as markdown list

-- ── Capability (stateful operations) ─────────────────────────────────────────

@capability markdown
  -- Streaming: incrementally parse a stream of string chunks
  stream_parse   (source: Stream str)            -> Stream MdPatch

  -- Editor state machine (immutable — every operation returns new state)
  editor_new     (content: str)                  -> MdEditorState
  editor_insert  (state: MdEditorState, text: str) -> MdEditorState
  editor_insert  (state: MdEditorState, text: str, at: int) -> MdEditorState
  editor_delete  (state: MdEditorState, range: MdRange) -> MdEditorState
  editor_replace (state: MdEditorState, range: MdRange, text: str) -> MdEditorState
  editor_format  (state: MdEditorState, format: atom) -> MdEditorState
  -- format: :bold | :italic | :code | :strikethrough | :link | :heading_1..6
  --         :bullet_list | :ordered_list | :blockquote | :code_block
  editor_format  (state: MdEditorState, format: atom, range: MdRange) -> MdEditorState
  editor_undo    (state: MdEditorState)          -> MdEditorState
  editor_redo    (state: MdEditorState)          -> MdEditorState
  editor_move    (state: MdEditorState, direction: atom, by: atom) -> MdEditorState
  -- direction: :forward | :backward; by: :char | :word | :line | :paragraph
  editor_select  (state: MdEditorState, range: MdRange) -> MdEditorState
  editor_select_all(state: MdEditorState)        -> MdEditorState
  editor_set_cursor(state: MdEditorState, offset: int) -> MdEditorState

  @errors
    :parse_failed  "Markdown parse error (malformed document)"
    :stream_closed "Source stream closed unexpectedly"

-- ── Events ────────────────────────────────────────────────────────────────────

@event markdown.link_tap    (url: str, text: str)
@event markdown.image_tap   (url: str, alt: str)
@event markdown.heading_enter (id: str, level: int, text: str)
@event markdown.heading_exit  (id: str)
-- heading_enter/exit fire as the user scrolls and headings enter/leave the viewport
```

### 2.1 `@include` in .deck-os

The main `.deck-os` file includes the markdown surface:

```
@os
  name:    "MyBoard v2"
  version: "2.0.0"

@include /etc/deck/markdown.deck-os

@capability sensors.temperature
  ...
```

`@include` splices the included file's declarations into the current file at that position. Included files may not themselves contain `@os` or `@include`.

---

## 3. View DSL Integration

The module registers two new component types with the runtime: `markdown` and `markdown_editor`. They appear in view bodies identically to built-in components.

### 3.1 `markdown` Component

```
markdown str_expr_or_doc
  -- Content (exactly one):
  -- positional: the string or MdDocument expression above

  -- Appearance
  style:       :normal | :compact | :dense | :prose
  theme:       MdTheme_expr?
  max_height:  int?          -- px; enables internal scroll when exceeded
  selectable:  bool          -- default true; false disables text selection

  -- Interactivity
  on link  -> expr           -- event.url, event.text available
  on image -> expr           -- event.url, event.alt available

  -- Navigation
  scroll_to:   str?          -- heading ID to scroll to; reactive
  show_toc:    bool          -- default false; renders inline ToC before content

  -- Code blocks
  code_copy:   bool          -- default true; copy button on code blocks
  code_theme:  atom?         -- :light | :dark | :auto (follows OS appearance)

  -- Images
  images:      bool          -- default true; false renders alt text only
  image_max_height: int?     -- px; clips tall images

  -- Performance
  virtual:     bool          -- default: auto (enabled for > 10 000 chars)
  -- virtual rendering: only renders nodes near the viewport; saves memory

  -- Accessibility
  accessibility: str?        -- overrides default label for the region
```

**Accepted content types:**
- `str` — parsed at render time; re-parsed when string changes
- `MdDocument` — pre-parsed; use when you parse once and render multiple times or in multiple places

```deck
-- Minimal:
markdown post.body

-- With options:
markdown content
  style:      :prose
  code_copy:  true
  on link ->
    match event.url |> text.starts("https://")
      | true  -> nav.push(:browser, url: event.url)
      | false -> unit

-- From pre-parsed document:
let doc = md.parse(raw_text)
markdown doc
  show_toc: true
  scroll_to: selected_heading_id
```

### 3.2 `markdown_editor` Component

A full markdown editing surface. Always controlled — the current content comes from state.

```
markdown_editor
  value:       str_expr           -- current content (from machine state)
  on change -> expr               -- event.value: str (new full content)
  on cursor -> expr               -- event.cursor: int, event.formats: [atom]
  on selection -> expr            -- event.selection: MdRange, event.text: str

  -- UI
  toolbar:     bool               -- default true: format toolbar above editor
  preview:     atom               -- :none | :side | :toggle (default :none)
  placeholder: str?
  min_lines:   int                -- minimum visible height (default 5)
  max_lines:   int?               -- beyond this, editor scrolls internally
  line_numbers:bool               -- default false

  -- Toolbar customization
  toolbar_items: [atom]?
  -- Atoms: :bold :italic :code :strikethrough :link :heading_1 :heading_2
  --        :heading_3 :bullet_list :ordered_list :blockquote :code_block
  --        :horizontal_rule :image :undo :redo :separator
  -- Default: all items except :image

  -- Editor state (for programmatic control via markdown.editor_* capability)
  editor_state: MdEditorState?    -- if provided, editor is fully externally controlled

  -- Accessibility
  label:        str?              -- accessible label for the editor region
```

**Minimal:**
```deck
markdown_editor
  value: state.content
  on change -> send(:update_content, text: event.value)
```

**Full external control:**
```deck
-- Machine owns the editor state, not just the string
markdown_editor
  editor_state: state.editor
  on change    -> send(:content_changed, text: event.value)
  on cursor    -> send(:cursor_moved, at: event.cursor, formats: event.formats)
  on selection -> send(:selection, range: event.selection)
  toolbar:       true
  toolbar_items: [:bold, :italic, :code, :separator, :heading_1, :heading_2,
                  :separator, :bullet_list, :ordered_list, :blockquote,
                  :separator, :undo, :redo]
  preview:       :side
```

### 3.3 Style Reference

| Atom | Description |
|------|-------------|
| `:normal` | Default reading style. Moderate spacing, proportional font. |
| `:compact` | Tighter line height and spacing. For dense information display. |
| `:dense` | Minimal margins, small font. For metadata, previews, changelogs. |
| `:prose` | Long-form reading: larger line height, generous paragraph spacing, constrained max-width. For articles and e-readers. |

Styles are composable at the `MdTheme` level for precise control.

---

## 4. The `md` Builtin Reference

Pure functions. Available everywhere without `@use`.

### 4.1 Parsing

```deck
let doc = md.parse(raw_text)
-- Returns MdDocument with .source, .nodes, .toc, .word_count, .image_urls

let doc = md.parse(raw_text, {
  "gfm_tables":      true,    -- GitHub Flavored Markdown tables (default: true)
  "gfm_tasks":       true,    -- - [ ] / - [x] task lists (default: true)
  "smart_quotes":    true,    -- "quotes" → "quotes" (default: false)
  "allow_html":      false,   -- inline HTML passthrough (default: false)
  "heading_ids":     true,    -- generate slug IDs for headings (default: true)
  "front_matter":    false    -- YAML front matter parsing (default: false)
})
```

### 4.2 Extraction and Transformation

```deck
md.to_plain("**Hello** _world_")
-- "Hello world"

md.to_html("**Hello**")
-- "<p><strong>Hello</strong></p>"

md.excerpt(content, 200)
-- first 200 chars, trimmed at word boundary, no markdown syntax

md.excerpt(content, 200, "…")
-- same, appends suffix if truncated

md.word_count(content)
-- int: counts words in rendered plain text, ignoring syntax

md.reading_time(content)
-- Duration: at 238 wpm (average adult reading speed)

md.reading_time(content, 180)
-- Duration: at custom wpm

md.headings(content)
-- [MdHeading]: [{level: 2, text: "Installation", id: "installation", ...}, ...]

md.heading_id("Getting Started!")
-- "getting-started"

md.extract_links(content)
-- [(text: "Anthropic", url: "https://anthropic.com"), ...]

md.extract_code(content)
-- [(lang: "python", code: "import os\n..."), (lang: "", code: "npm install")]

md.front_matter(content)
-- {str: any}: {"title": "Post", "date": "2025-01-01", "tags": ["deck", "embedded"]}

md.body_after_front_matter(content)
-- str: everything after the --- ... --- front matter block

md.sanitize(content)
-- strips HTML; leaves all markdown syntax intact

md.sanitize(content, true)
-- strips only dangerous HTML (script, iframe, object); allows safe HTML
```

### 4.3 AST Traversal

```deck
let doc = md.parse(content)

-- Walk the top-level nodes
for node in doc.nodes
  match node.type
    | :heading ->
        text "H{unwrap_opt_or(node.level, 1)}: {unwrap_opt_or(node.text, "")}"
    | :code_block ->
        let lang = unwrap_opt_or(node.lang, "text")
        text "Code block ({lang})"
    | _ -> unit

-- Get all headings
let toc = md.headings(doc)

-- Get all code blocks of a specific language
let py_blocks = doc.nodes
  |> filter(n -> n.type == :code_block and n.lang == :some "python")
  |> map(n -> unwrap_opt_or(n.text, ""))

-- Recursive node text (full text content of a node and all children)
let plain = md.node_text(some_node)

-- Generate a ToC as markdown (for embedding in another document)
let toc_md = md.toc_markdown(doc)
-- "- [Introduction](#introduction)\n  - [Background](#background)\n..."
```

### 4.4 Front Matter

For note-taking and e-reader apps, YAML-like front matter is common:

```
---
title: "My Note"
tags: [work, project-x]
created: 2025-03-15
pinned: true
---

Content starts here.
```

```deck
let has_meta  = md.has_front_matter(raw)
let meta      = md.front_matter(raw)    -- {str: any}
let body      = md.body_after_front_matter(raw)

let title     = unwrap_opt_or(row.str(meta, "title"), "Untitled")
let tags      = match map.get(meta, "tags")
  | :some v when is_list(v) -> map(v, str)
  | _                       -> []
let is_pinned = unwrap_opt_or(row.bool(meta, "pinned"), false)
```

---

## 5. Streaming Markdown

For AI chat responses, live document sync, or streaming file reads.

### 5.1 Pattern

```deck
@use
  markdown as md_cap
  ./api/ai

@stream AiTokens
  source: ai.generate_stream(prompt)

@stream RenderedDoc
  from:   AiTokens
  -- fold tokens into an accumulating string, then stream MdPatch values

-- In view:
@machine ChatView
  state :empty
  state :streaming (accumulated: str)
  state :done      (document: MdDocument)
  initial :empty

  transition :patch (patch: MdPatch)
    from :empty
    to   :streaming (accumulated: patch.text)
  transition :patch (patch: MdPatch)
    from :streaming s
    to   :streaming (accumulated: "{s.accumulated}{patch.text}")
  transition :finalize (doc: MdDocument)
    from :streaming _
    to   :done (document: doc)

  body =
    screen
      scroll
        match state
          | :empty      -> spinner
          | :streaming s -> markdown s.accumulated
          | :done s     -> markdown s.document
```

The `md_cap.stream_parse` capability handles the accumulation and incremental parsing:

```deck
@stream ParsedResponse
  source: md_cap.stream_parse(AiTokens)
  -- source: Stream str (tokens) → Stream MdPatch
```

`MdPatch` values:
- `:append` — a few new tokens arrived; `text` field has them
- `:replace` — a block completed and was reparsed; `document` has the revised doc so far
- `:finalize` — stream ended; `document` has the complete final parse

The `markdown` component handles `MdPatch` internally when given a stream source:

```deck
markdown ParsedResponse.last()
  -- Works with both str, MdDocument, and MdPatch optionals
  -- Renders progressively; completed blocks lock in; typing cursor at end
```

---

## 6. C Bridge Implementation

### 6.1 Dependencies

This module uses [md4c](https://github.com/mity/md4c) — a CommonMark + GFM compliant C parser designed for embedded use. It is header-only-compatible, allocates no global state, and processes documents in a single pass.

```c
/* Required: */
#include "md4c.h"
#include "deck_bridge.h"
```

### 6.2 Parser Pipeline

```
Source str (DeckValue*)
    │
    ▼
md4c parser (MD_PARSER callbacks)
    │
    ▼
MdNodeBuilder (C stack-based tree builder)
    │
    ▼
DeckValue* of type MdDocument (@type record)
    │
    ├──→ stored as MdDocument for AST access
    └──→ passed to ComponentBuilder for rendering
```

### 6.3 md4c Callback Context

```c
typedef struct MdParseCtx {
    /* Runtime */
    DeckRuntime*  rt;

    /* Node stack */
    MdNodeEntry*  stack;
    int           stack_top;
    int           stack_cap;

    /* String accumulator for text content */
    char*         text_buf;
    size_t        text_len;
    size_t        text_cap;

    /* Table of contents accumulator */
    DeckValue**   toc_items;
    size_t        toc_count;
    size_t        toc_cap;

    /* Image URL accumulator */
    DeckValue**   image_urls;
    size_t        img_count;

    /* Parse options */
    bool          allow_html;
    bool          heading_ids;
    bool          smart_quotes;
    bool          gfm_tasks;

    /* Word count (updated during text processing) */
    int           word_count;
    bool          in_code;
} MdParseCtx;

typedef struct MdNodeEntry {
    const char*   type;       /* atom name: "heading", "paragraph", etc. */
    int           level;      /* heading level, or 0 */
    char*         lang;       /* code block language, or NULL */
    char*         url;        /* link/image URL, or NULL */
    char*         alt;        /* image alt text, or NULL */
    char*         title;      /* link title, or NULL */
    bool          ordered;
    bool          tight;
    int           checked;    /* -1 = not task, 0 = unchecked, 1 = checked */
    int           align;      /* 0=none 1=left 2=center 3=right */
    bool          is_header;
    DeckValue**   children;
    size_t        child_count;
    size_t        child_cap;
} MdNodeEntry;
```

### 6.4 Parser Callbacks

```c
static int on_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    MdParseCtx* ctx = (MdParseCtx*)userdata;
    push_node(ctx);
    MdNodeEntry* node = top_node(ctx);

    switch (type) {
        case MD_BLOCK_H: {
            MD_BLOCK_H_DETAIL* d = (MD_BLOCK_H_DETAIL*)detail;
            node->type  = "heading";
            node->level = d->level;
            break;
        }
        case MD_BLOCK_P:  node->type = "paragraph";   break;
        case MD_BLOCK_CODE: {
            MD_BLOCK_CODE_DETAIL* d = (MD_BLOCK_CODE_DETAIL*)detail;
            node->type = "code_block";
            node->lang = d->lang.size > 0
                ? strndup(d->lang.text, d->lang.size) : NULL;
            break;
        }
        case MD_BLOCK_QUOTE:  node->type = "blockquote";  break;
        case MD_BLOCK_UL:     node->type = "list";
                               node->ordered = false;
                               node->tight = ((MD_BLOCK_UL_DETAIL*)detail)->is_tight;
                               break;
        case MD_BLOCK_OL:     node->type = "list";
                               node->ordered = true;
                               node->tight = ((MD_BLOCK_OL_DETAIL*)detail)->is_tight;
                               break;
        case MD_BLOCK_LI: {
            MD_BLOCK_LI_DETAIL* d = (MD_BLOCK_LI_DETAIL*)detail;
            node->type    = "list_item";
            node->checked = d->is_task ? (d->task_mark == 'x' ? 1 : 0) : -1;
            break;
        }
        case MD_BLOCK_TABLE:  node->type = "table";       break;
        case MD_BLOCK_THEAD:
        case MD_BLOCK_TBODY:  /* transparent — children go to table */
                               node->type = "table_section";
                               break;
        case MD_BLOCK_TR:     node->type = "table_row";   break;
        case MD_BLOCK_TH: {
            MD_BLOCK_TD_DETAIL* d = (MD_BLOCK_TD_DETAIL*)detail;
            node->type      = "table_cell";
            node->align     = d->align;
            node->is_header = true;
            break;
        }
        case MD_BLOCK_TD: {
            MD_BLOCK_TD_DETAIL* d = (MD_BLOCK_TD_DETAIL*)detail;
            node->type      = "table_cell";
            node->align     = d->align;
            node->is_header = false;
            break;
        }
        case MD_BLOCK_HR:     node->type = "hr";           break;
        case MD_BLOCK_HTML:   node->type = "html_block";
                               if (!ctx->allow_html) node->type = "stripped_html";
                               break;
        default:               node->type = "unknown";     break;
    }
    return 0;
}

static int on_leave_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    MdParseCtx* ctx = (MdParseCtx*)userdata;
    MdNodeEntry* node = top_node(ctx);

    /* For headings: flush text, generate heading ID, add to ToC */
    if (type == MD_BLOCK_H) {
        node->text = flush_text(ctx);
        if (ctx->heading_ids) {
            char* id = slugify(node->text);
            add_toc_entry(ctx, node->level, node->text, id);
            free(id);
        }
    }
    /* For code blocks: flush accumulated text as code content */
    if (type == MD_BLOCK_CODE) {
        node->text = flush_text(ctx);
    }
    /* For transparent sections (thead/tbody): lift children to parent */
    if (type == MD_BLOCK_THEAD || type == MD_BLOCK_TBODY) {
        lift_children_to_parent(ctx);
        return 0;
    }

    DeckValue* node_val = finish_node(ctx);
    pop_and_add_child(ctx, node_val);
    return 0;
}

static int on_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    MdParseCtx* ctx = (MdParseCtx*)userdata;
    if (type == MD_TEXT_NULLCHAR) return 0;

    append_text(ctx, text, size);

    /* Word count: count in non-code text */
    if (!ctx->in_code && type == MD_TEXT_NORMAL) {
        ctx->word_count += count_words(text, size);
    }
    return 0;
}
```

### 6.5 Building MdDocument from Parsed Tree

```c
static DeckValue* build_md_document(MdParseCtx* ctx, const char* source) {
    /* Build nodes list from root children */
    DeckValue* nodes_list = deck_list(ctx->root_children, ctx->root_child_count);

    /* Build ToC list */
    DeckValue* toc_list = deck_list(ctx->toc_items, ctx->toc_count);

    /* Build image_urls list */
    DeckValue* imgs_list = deck_list(ctx->image_urls, ctx->img_count);

    const char* field_names[] = {
        "source", "nodes", "toc", "word_count", "image_urls"
    };
    DeckValue* field_vals[] = {
        deck_str(source),
        nodes_list,
        toc_list,
        deck_int(ctx->word_count),
        imgs_list
    };

    DeckValue* doc = deck_record("MdDocument", field_names, field_vals, 5);

    for (int i = 0; i < 5; i++) deck_value_free(field_vals[i]);
    return doc;
}
```

### 6.6 Building an MdNode Record

```c
static DeckValue* build_node_record(MdNodeEntry* e) {
    /* Fields are optional — use :none for absent ones */
    #define OPT_STR(s)   ((s) ? deck_some(deck_str(s)) : deck_none())
    #define OPT_INT(n,v) ((n) ? deck_some(deck_int(v)) : deck_none())
    #define OPT_BOOL(b)  ((b) >= 0 ? deck_some(deck_bool(b)) : deck_none())

    DeckValue* children = e->child_count > 0
        ? deck_list(e->children, e->child_count)
        : deck_list_empty();

    const char* field_names[] = {
        "type", "level", "text", "lang", "url", "alt", "title",
        "ordered", "tight", "checked", "align", "is_header", "children"
    };
    DeckValue* field_vals[] = {
        deck_atom(e->type),
        OPT_INT(e->level != 0, e->level),
        OPT_STR(e->text),
        OPT_STR(e->lang),
        OPT_STR(e->url),
        OPT_STR(e->alt),
        OPT_STR(e->title),
        e->type[0] == 'l' ? deck_some(deck_bool(e->ordered)) : deck_none(),
        e->type[0] == 'l' ? deck_some(deck_bool(e->tight))   : deck_none(),
        OPT_BOOL(e->checked),
        e->align > 0 ? deck_some(deck_atom(align_atom(e->align))) : deck_none(),
        e->type[0] == 't' ? deck_some(deck_bool(e->is_header)) : deck_none(),
        children
    };

    DeckValue* record = deck_record("MdNode", field_names, field_vals, 13);
    for (int i = 0; i < 13; i++) deck_value_free(field_vals[i]);
    deck_value_free(children);

    #undef OPT_STR
    #undef OPT_INT
    #undef OPT_BOOL
    return record;
}
```

### 6.7 The `md.parse` Builtin

```c
static DeckValue* md_parse_builtin(DeckRuntime* rt, DeckValue** args, size_t argc) {
    const char* source = deck_get_str(args[0]);

    /* Options from optional second argument */
    bool allow_html    = false;
    bool heading_ids   = true;
    bool smart_quotes  = false;
    bool gfm_tasks     = true;
    bool gfm_tables    = true;

    if (argc >= 2 && deck_is_map(args[1])) {
        DeckValue* v;
        if ((v = deck_map_get(args[1], "allow_html"))   && deck_is_bool(v)) allow_html   = deck_get_bool(v);
        if ((v = deck_map_get(args[1], "heading_ids"))  && deck_is_bool(v)) heading_ids  = deck_get_bool(v);
        if ((v = deck_map_get(args[1], "smart_quotes")) && deck_is_bool(v)) smart_quotes = deck_get_bool(v);
        if ((v = deck_map_get(args[1], "gfm_tasks"))    && deck_is_bool(v)) gfm_tasks    = deck_get_bool(v);
        if ((v = deck_map_get(args[1], "gfm_tables"))   && deck_is_bool(v)) gfm_tables   = deck_get_bool(v);
    }

    MdParseCtx ctx = {
        .rt           = rt,
        .allow_html   = allow_html,
        .heading_ids  = heading_ids,
        .smart_quotes = smart_quotes,
        .gfm_tasks    = gfm_tasks,
    };
    md_parse_ctx_init(&ctx);

    MD_PARSER parser = {
        .flags          = 0
                        | (gfm_tables  ? MD_FLAG_TABLES       : 0)
                        | (gfm_tasks   ? MD_FLAG_TASKLISTS     : 0)
                        | (smart_quotes? MD_FLAG_PERMISSIVEURLAUTOLINKS: 0)
                        | (!allow_html ? MD_FLAG_NOHTML        : 0),
        .enter_block    = on_enter_block,
        .leave_block    = on_leave_block,
        .enter_span     = on_enter_span,
        .leave_span     = on_leave_span,
        .text           = on_text,
        .debug_log      = NULL,
        .syntax         = NULL,
    };

    int err = md_parse(source, (MD_SIZE)strlen(source), &parser, &ctx);
    if (err != 0) {
        md_parse_ctx_free(&ctx);
        /* Never fail — return empty document on parse error */
        return build_empty_document(rt, source);
    }

    DeckValue* doc = build_md_document(&ctx, source);
    md_parse_ctx_free(&ctx);
    return doc;
}
```

### 6.8 Registering the Custom `markdown` Component Type

The `markdown` and `markdown_editor` components are not built into the interpreter. They are registered as **custom component types** via a C API that maps a component name to a C render function:

```c
/* deck_bridge.h — component type extension */

typedef ComponentNode* (*DeckComponentRenderFn)(
    DeckRuntime*          rt,
    const ComponentProps* props,    /* the component's prop map */
    uint64_t              component_id
);

void deck_register_component_type(
    DeckRuntime*            rt,
    const char*             type_name,      /* "markdown", "markdown_editor" */
    DeckComponentRenderFn   render_fn,
    const char**            required_props, /* NULL-terminated; loader validates */
    const char**            optional_props
);
```

The `render_fn` receives the component's props as evaluated `DeckValue*` values (content string, style atom, etc.) and returns a `ComponentNode*` subtree — identical to what the built-in `column`, `text`, `image` components produce. The OS renderer never knows the difference.

```c
static ComponentNode* markdown_render(
    DeckRuntime*          rt,
    const ComponentProps* props,
    uint64_t              component_id
) {
    /* Extract content — either str or MdDocument */
    DeckValue* content = component_props_get(props, "content");
    DeckValue* doc;
    bool       we_own_doc = false;

    if (deck_is_str(content)) {
        doc = md_parse_builtin(rt, &content, 1);
        we_own_doc = true;
    } else if (deck_is_record(content) &&
               strcmp(deck_record_type(content), "MdDocument") == 0) {
        doc = content;
    } else {
        return component_node_text("[invalid markdown content]");
    }

    /* Extract style */
    DeckValue* style_val = component_props_get(props, "style");
    MdStyle    style = style_val ? decode_md_style(deck_get_atom(style_val))
                                 : MD_STYLE_NORMAL;

    /* Extract theme */
    DeckValue*  theme_val = component_props_get(props, "theme");
    MdThemeData theme = theme_val ? decode_md_theme(theme_val) : default_theme();

    /* Extract options */
    bool show_toc   = component_props_bool(props, "show_toc",   false);
    bool code_copy  = component_props_bool(props, "code_copy",  true);
    bool images_on  = component_props_bool(props, "images",     true);
    bool virtual_scroll = component_props_bool(props, "virtual", false);
    int  max_height = component_props_int(props, "max_height",  0);

    /* Check if virtual rendering is appropriate */
    if (!virtual_scroll) {
        DeckValue* source = deck_record_get(doc, "source");
        if (source && deck_get_str_len(source) > 10000) {
            virtual_scroll = true;
        }
    }

    /* Build component tree */
    ComponentNodeBuilder* b = component_builder_new(rt);

    if (show_toc) {
        DeckValue* toc = deck_record_get(doc, "toc");
        component_builder_append(b, render_toc(rt, toc, style));
    }

    DeckValue* nodes = deck_record_get(doc, "nodes");
    size_t     count = deck_list_count(nodes);

    if (virtual_scroll) {
        /* Wrap in virtual scroller — renders a window of nodes */
        ComponentNode* virtual_col = component_node_virtual_column(
            rt, component_id, count,
            /* item renderer callback: */
            render_md_node_at,
            /* context: */
            nodes, &theme, style, code_copy, images_on
        );
        component_builder_append(b, virtual_col);
    } else {
        for (size_t i = 0; i < count; i++) {
            DeckValue* node = deck_list_at(nodes, i);
            ComponentNode* cn = render_md_node(rt, node, &theme, style,
                                               code_copy, images_on,
                                               component_id, i);
            component_builder_append(b, cn);
        }
    }

    if (max_height > 0) {
        ComponentNode* result = component_node_clipped_scroll(
            b, max_height, component_id
        );
        if (we_own_doc) deck_value_free(doc);
        return result;
    }

    ComponentNode* result = component_builder_finish(b);
    if (we_own_doc) deck_value_free(doc);
    return result;
}
```

### 6.9 Rendering Individual Nodes

```c
static ComponentNode* render_md_node(
    DeckRuntime* rt, DeckValue* node, MdThemeData* theme,
    MdStyle style, bool code_copy, bool images,
    uint64_t parent_id, size_t index
) {
    const char* type = deck_get_atom(deck_record_get(node, "type"));

    if (strcmp(type, "heading") == 0) {
        return render_heading(rt, node, theme, style);
    }
    if (strcmp(type, "paragraph") == 0) {
        return render_paragraph(rt, node, theme, style);
    }
    if (strcmp(type, "code_block") == 0) {
        return render_code_block(rt, node, theme, code_copy);
    }
    if (strcmp(type, "blockquote") == 0) {
        return render_blockquote(rt, node, theme, style, code_copy, images);
    }
    if (strcmp(type, "list") == 0) {
        return render_list(rt, node, theme, style, code_copy, images);
    }
    if (strcmp(type, "table") == 0) {
        return render_table(rt, node, theme, style);
    }
    if (strcmp(type, "hr") == 0) {
        return component_node_divider();
    }
    if (strcmp(type, "image") == 0) {
        return images ? render_image(rt, node, theme)
                      : render_image_alt(rt, node);
    }
    return component_node_text("");  /* unknown node type — render nothing */
}

static ComponentNode* render_heading(DeckRuntime* rt, DeckValue* node,
                                      MdThemeData* theme, MdStyle style) {
    DeckValue* text_v = deck_record_get(node, "text");
    DeckValue* level_v = deck_record_get(node, "level");
    const char* text  = (text_v && deck_is_some(text_v))
        ? deck_get_str(deck_get_some_val(text_v)) : "";
    int level = (level_v && deck_is_some(level_v))
        ? (int)deck_get_int(deck_get_some_val(level_v)) : 1;

    /* Map heading level → OS text style */
    const char* text_style = heading_level_to_style(level, style);

    ComponentNode* cn = component_node_text(text);
    component_node_set_style(cn, text_style);
    component_node_set_font_scale(cn, heading_scale(level, theme));

    /* Register heading ID for scroll-to support */
    DeckValue* id_v = heading_id_from_text(text);
    component_node_set_prop(cn, "heading_id", id_v);
    deck_value_free(id_v);

    return cn;
}

static ComponentNode* render_code_block(DeckRuntime* rt, DeckValue* node,
                                          MdThemeData* theme, bool copy_btn) {
    DeckValue* text_v = deck_record_get(node, "text");
    DeckValue* lang_v = deck_record_get(node, "lang");
    const char* code = (text_v && deck_is_some(text_v))
        ? deck_get_str(deck_get_some_val(text_v)) : "";
    const char* lang = (lang_v && deck_is_some(lang_v))
        ? deck_get_str(deck_get_some_val(lang_v)) : "";

    ComponentNodeBuilder* b = component_builder_new(rt);

    /* Language label */
    if (strlen(lang) > 0) {
        ComponentNode* lang_label = component_node_text(lang);
        component_node_set_style(lang_label, "caption muted");
        component_builder_append(b, lang_label);
    }

    /* Code text */
    ComponentNode* code_text = component_node_text(code);
    component_node_set_style(code_text, "code");
    component_node_set_font(code_text, theme->code_font);
    component_node_set_selectable(code_text, true);
    component_builder_append(b, code_text);

    /* Copy button */
    if (copy_btn) {
        ComponentNode* copy = component_node_button("Copy");
        component_node_set_style(copy, "ghost small");
        component_node_set_action_copy(copy, code);
        component_builder_append(b, copy);
    }

    ComponentNode* card = component_node_card(component_builder_finish(b));
    component_node_set_bg(card, theme->code_bg);
    return card;
}

static ComponentNode* render_image(DeckRuntime* rt, DeckValue* node,
                                     MdThemeData* theme) {
    DeckValue* url_v = deck_record_get(node, "url");
    DeckValue* alt_v = deck_record_get(node, "alt");
    const char* url  = (url_v && deck_is_some(url_v)) ? deck_get_str(deck_get_some_val(url_v)) : "";
    const char* alt  = (alt_v && deck_is_some(alt_v)) ? deck_get_str(deck_get_some_val(alt_v)) : "";

    ComponentNode* img = component_node_image(url, alt);
    component_node_set_style(img, "full_width");
    if (theme->image_max_height > 0) {
        component_node_set_max_height(img, theme->image_max_height);
    }
    /* Tap event */
    component_node_set_tap_event(img, "markdown.image_tap",
                                  "url", url, "alt", alt, NULL);
    return img;
}
```

### 6.10 Streaming Render Capability

```c
typedef struct StreamParseCtx {
    DeckRuntime* rt;
    uint64_t     sub_id;
    char*        accumulated;  /* growing string */
    size_t       acc_len;
    size_t       acc_cap;
    MdParseCtx   parse_ctx;
    int          token_count;  /* incremental reparse threshold */
} StreamParseCtx;

static uint64_t md_stream_parse_start(
    DeckRuntime* rt, DeckValue** args, size_t argc,
    const char** nk, DeckValue** nv, size_t nc
) {
    uint64_t sub_id = deck_new_subscription_id(rt);

    StreamParseCtx* ctx = calloc(1, sizeof(StreamParseCtx));
    ctx->rt     = rt;
    ctx->sub_id = sub_id;
    ctx->acc_cap = 4096;
    ctx->accumulated = malloc(ctx->acc_cap);
    ctx->accumulated[0] = '\0';
    md_parse_ctx_init(&ctx->parse_ctx);
    deck_subscription_set_ctx(rt, sub_id, ctx);

    /* Subscribe to the source stream (args[0] is the Stream str) */
    deck_stream_subscribe(rt, args[0], sub_id, on_md_token, on_md_stream_end, ctx);
    return sub_id;
}

static void on_md_token(DeckRuntime* rt, uint64_t sub_id, DeckValue* token, void* user) {
    StreamParseCtx* ctx = (StreamParseCtx*)user;
    const char* text = deck_get_str(token);
    size_t      tlen = deck_get_str_len(token);

    /* Append to accumulator */
    if (ctx->acc_len + tlen + 1 > ctx->acc_cap) {
        ctx->acc_cap = (ctx->acc_len + tlen + 1) * 2;
        ctx->accumulated = realloc(ctx->accumulated, ctx->acc_cap);
    }
    memcpy(ctx->accumulated + ctx->acc_len, text, tlen);
    ctx->acc_len += tlen;
    ctx->accumulated[ctx->acc_len] = '\0';
    ctx->token_count++;

    /* Emit :append patch immediately (fast, no parse) */
    const char* names_append[] = { "type", "text", "document" };
    DeckValue*  vals_append[]  = {
        deck_atom("append"),
        deck_str_n(text, tlen),
        deck_none()
    };
    DeckValue* append_patch = deck_record("MdPatch", names_append, vals_append, 3);
    for (int i = 0; i < 3; i++) deck_value_free(vals_append[i]);
    deck_stream_push(rt, ctx->sub_id, append_patch);
    deck_value_free(append_patch);

    /* Incremental reparse every 50 tokens (for block-complete updates) */
    if (ctx->token_count % 50 == 0) {
        DeckValue* src  = deck_str(ctx->accumulated);
        DeckValue* doc  = md_parse_builtin(rt, &src, 1);
        deck_value_free(src);

        const char* names_replace[] = { "type", "text", "document" };
        DeckValue*  vals_replace[]  = {
            deck_atom("replace"),
            deck_none(),
            deck_some(doc)
        };
        DeckValue* replace_patch = deck_record("MdPatch", names_replace, vals_replace, 3);
        for (int i = 0; i < 3; i++) deck_value_free(vals_replace[i]);
        deck_value_free(doc);
        deck_stream_push(rt, ctx->sub_id, replace_patch);
        deck_value_free(replace_patch);
    }
}

static void on_md_stream_end(DeckRuntime* rt, uint64_t sub_id, void* user) {
    StreamParseCtx* ctx = (StreamParseCtx*)user;

    /* Final full parse */
    DeckValue* src = deck_str(ctx->accumulated);
    DeckValue* doc = md_parse_builtin(rt, &src, 1);
    deck_value_free(src);

    const char* names[] = { "type", "text", "document" };
    DeckValue*  vals[]  = {
        deck_atom("finalize"),
        deck_none(),
        deck_some(doc)
    };
    DeckValue* final_patch = deck_record("MdPatch", names, vals, 3);
    for (int i = 0; i < 3; i++) deck_value_free(vals[i]);
    deck_value_free(doc);

    deck_stream_push(rt, ctx->sub_id, final_patch);
    deck_value_free(final_patch);
    deck_stream_end(rt, ctx->sub_id);

    free(ctx->accumulated);
    md_parse_ctx_free(&ctx->parse_ctx);
    free(ctx);
}
```

### 6.11 Editor Capability Implementation

```c
/* Editor operations work on MdEditorState @type records — immutable.
   Each operation takes a state record and returns a new state record. */

static DeckValue* md_editor_new(DeckRuntime* rt, DeckValue** args, size_t argc,
                                  const char** nk, DeckValue** nv, size_t nc) {
    const char* content = deck_get_str(args[0]);

    const char* names[] = {
        "content", "cursor", "selection", "history_len", "active_formats"
    };
    DeckValue* vals[] = {
        deck_str(content),
        deck_int(0),         /* cursor at start */
        deck_none(),         /* no selection */
        deck_int(0),
        deck_list_empty()    /* no active formats */
    };
    DeckValue* state = deck_record("MdEditorState", names, vals, 5);
    for (int i = 0; i < 5; i++) deck_value_free(vals[i]);
    return state;
}

static DeckValue* md_editor_insert(DeckRuntime* rt, DeckValue** args, size_t argc,
                                     const char** nk, DeckValue** nv, size_t nc) {
    DeckValue* state   = args[0];
    const char* insert = deck_get_str(args[1]);

    DeckValue* cursor_v  = deck_record_get(state, "cursor");
    DeckValue* content_v = deck_record_get(state, "content");

    int64_t     at      = argc >= 3 ? deck_get_int(args[2])
                                    : deck_get_int(cursor_v);
    const char* content = deck_get_str(content_v);
    size_t      clen    = strlen(content);
    size_t      ilen    = strlen(insert);

    /* Bounds check */
    if (at < 0) at = 0;
    if ((size_t)at > clen) at = (int64_t)clen;

    /* Build new content string */
    char* new_content = malloc(clen + ilen + 1);
    memcpy(new_content, content, at);
    memcpy(new_content + at, insert, ilen);
    memcpy(new_content + at + ilen, content + at, clen - at);
    new_content[clen + ilen] = '\0';

    /* Infer active formats at new cursor position */
    DeckValue* new_content_val = deck_str(new_content);
    int64_t new_cursor = at + (int64_t)ilen;
    DeckValue* formats = infer_active_formats(new_content, new_cursor);
    free(new_content);

    /* Build new MdEditorState with incremented history_len */
    DeckValue* history_v = deck_record_get(state, "history_len");
    const char* names[] = {
        "content", "cursor", "selection", "history_len", "active_formats"
    };
    DeckValue* vals[] = {
        new_content_val,
        deck_int(new_cursor),
        deck_none(),    /* insertion clears selection */
        deck_int(deck_get_int(history_v) + 1),
        formats
    };
    DeckValue* new_state = deck_record("MdEditorState", names, vals, 5);
    for (int i = 0; i < 5; i++) deck_value_free(vals[i]);
    return new_state;
}

static DeckValue* md_editor_format(DeckRuntime* rt, DeckValue** args, size_t argc,
                                     const char** nk, DeckValue** nv, size_t nc) {
    DeckValue*  state   = args[0];
    const char* format  = deck_get_atom(args[1]);

    DeckValue* content_v   = deck_record_get(state, "content");
    DeckValue* cursor_v    = deck_record_get(state, "cursor");
    DeckValue* selection_v = deck_record_get(state, "selection");
    const char* content = deck_get_str(content_v);
    int64_t cursor      = deck_get_int(cursor_v);

    /* Determine range: use selection if present, else infer word boundary */
    MdRange range;
    if (deck_is_some(selection_v)) {
        DeckValue* sel = deck_get_some_val(selection_v);
        range.start = (size_t)deck_get_int(deck_record_get(sel, "start"));
        range.end   = (size_t)deck_get_int(deck_record_get(sel, "end"));
    } else {
        range = infer_word_range(content, (size_t)cursor);
    }

    if (argc >= 3) {
        /* Explicit range provided as third arg */
        DeckValue* r = args[2];
        range.start = (size_t)deck_get_int(deck_record_get(r, "start"));
        range.end   = (size_t)deck_get_int(deck_record_get(r, "end"));
    }

    char* new_content = apply_format(content, format, range);
    if (!new_content) return state;  /* no-op: format not applicable */

    DeckValue* new_content_v = deck_str(new_content);
    DeckValue* formats = infer_active_formats(new_content, cursor);
    free(new_content);

    const char* names[] = {
        "content", "cursor", "selection", "history_len", "active_formats"
    };
    DeckValue* history_v = deck_record_get(state, "history_len");
    DeckValue* vals[] = {
        new_content_v,
        deck_int(cursor),
        selection_v,
        deck_int(deck_get_int(history_v) + 1),
        formats
    };
    DeckValue* new_state = deck_record("MdEditorState", names, vals, 5);
    for (int i = 0; i < 5; i++) deck_value_free(vals[i]);
    return new_state;
}
```

### 6.12 Full Registration

```c
void markdown_module_register(DeckRuntime* rt) {

    /* ── Builtins ─────────────────────────────────────────────────────────── */
    deck_register_builtin(rt, "md", "parse",          md_parse_builtin,    false);
    deck_register_builtin(rt, "md", "to_plain",       md_to_plain,         false);
    deck_register_builtin(rt, "md", "to_html",        md_to_html,          false);
    deck_register_builtin(rt, "md", "excerpt",        md_excerpt,          false);
    deck_register_builtin(rt, "md", "word_count",     md_word_count,       false);
    deck_register_builtin(rt, "md", "reading_time",   md_reading_time,     false);
    deck_register_builtin(rt, "md", "headings",       md_headings,         false);
    deck_register_builtin(rt, "md", "heading_id",     md_heading_id,       false);
    deck_register_builtin(rt, "md", "strip_images",   md_strip_images,     false);
    deck_register_builtin(rt, "md", "extract_links",  md_extract_links,    false);
    deck_register_builtin(rt, "md", "extract_code",   md_extract_code,     false);
    deck_register_builtin(rt, "md", "has_front_matter",md_has_front_matter,false);
    deck_register_builtin(rt, "md", "front_matter",   md_front_matter,     false);
    deck_register_builtin(rt, "md", "body_after_front_matter",
                                                      md_body_after_fm,    false);
    deck_register_builtin(rt, "md", "sanitize",       md_sanitize,         false);
    deck_register_builtin(rt, "md", "node_text",      md_node_text,        false);
    deck_register_builtin(rt, "md", "node_children",  md_node_children,    false);
    deck_register_builtin(rt, "md", "toc_markdown",   md_toc_markdown,     false);

    /* ── Capability ───────────────────────────────────────────────────────── */
    deck_register_stream    (rt, "markdown", "stream_parse",
                             md_stream_parse_start, md_stream_parse_stop);
    deck_register_capability(rt, "markdown", "editor_new",
                             md_editor_new,     false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_insert",
                             md_editor_insert,  false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_delete",
                             md_editor_delete,  false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_replace",
                             md_editor_replace, false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_format",
                             md_editor_format,  false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_undo",
                             md_editor_undo,    false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_redo",
                             md_editor_redo,    false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_move",
                             md_editor_move,    false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_select",
                             md_editor_select,  false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_select_all",
                             md_editor_select_all, false, NULL, NULL);
    deck_register_capability(rt, "markdown", "editor_set_cursor",
                             md_editor_set_cursor, false, NULL, NULL);

    /* ── Events ───────────────────────────────────────────────────────────── */
    deck_register_event(rt, "markdown.link_tap");
    deck_register_event(rt, "markdown.image_tap");
    deck_register_event(rt, "markdown.heading_enter");
    deck_register_event(rt, "markdown.heading_exit");

    /* ── Component types ──────────────────────────────────────────────────── */
    const char* md_required[] = { NULL };
    const char* md_optional[] = {
        "style", "theme", "max_height", "selectable",
        "on_link", "on_image", "scroll_to", "show_toc",
        "code_copy", "code_theme", "images", "image_max_height",
        "virtual", "accessibility", NULL
    };
    deck_register_component_type(rt, "markdown",
                                  markdown_render, md_required, md_optional);

    const char* ed_required[] = { "value", NULL };
    const char* ed_optional[] = {
        "on_change", "on_cursor", "on_selection",
        "toolbar", "toolbar_items", "preview", "placeholder",
        "min_lines", "max_lines", "line_numbers", "editor_state",
        "label", NULL
    };
    deck_register_component_type(rt, "markdown_editor",
                                  markdown_editor_render, ed_required, ed_optional);

    /* ── Type metadata ────────────────────────────────────────────────────── */
    deck_register_type(rt, "MdDocument", ...);
    deck_register_type(rt, "MdNode",     ...);
    deck_register_type(rt, "MdHeading",  ...);
    deck_register_type(rt, "MdRange",    ...);
    deck_register_type(rt, "MdPatch",    ...);
    deck_register_type(rt, "MdEditorState", ...);
    deck_register_type(rt, "MdTheme",    ...);
}
```

---

## 7. Complete App Examples

### 7.1 Note Taker

```deck
-- app.deck

@app
  name:    "Notes"
  id:      "mx.lab.notes"
  version: "1.0.0"

@use
  db            as db
  markdown      as md_cap
  ./models/note
  ./views/note_list
  ./views/note_editor
  ./views/note_reader
  ./views/search_view

@permissions
  db reason: "Store your notes locally"

@config
  editor_font_size : int   = 16     range: 12..24  unit: "pt"
  preview_on_edit  : bool  = true
  default_style    : atom  = :prose options: [:prose, :normal, :compact]

@machine App
  state :list
  state :editing  (note: Note, mode: atom)  -- mode: :edit | :preview | :split
  state :reading  (note: Note)
  state :new_note (content: str)
  state :searching (query: str)

  initial :list

  transition :open_note (note: Note)
    from :list
    to   :reading (note: note)

  transition :edit_note (note: Note)
    from :list
    from :reading _
    to   :editing (note: note, mode: :edit)

  transition :new
    from :list
    to   :new_note (content: "")

  transition :save (note: Note)
    from :editing _
    from :new_note _
    to   :reading (note: note)

  transition :discard  from :editing _   to :list
  transition :discard  from :new_note _  to :list

  transition :toggle_mode
    from :editing s
    to   :editing (note: s.note,
                   mode: match s.mode | :edit -> :split | :split -> :preview | :preview -> :edit)

  transition :back  from :reading _   to :list
  transition :search (query: str)
    from :list
    to   :searching (query: query)
  transition :end_search  from :searching _  to :list

@on launch
  db.exec("""
    CREATE TABLE IF NOT EXISTS notes (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      title      TEXT    NOT NULL,
      content    TEXT    NOT NULL DEFAULT '',
      tags       TEXT    NOT NULL DEFAULT '[]',
      pinned     INTEGER NOT NULL DEFAULT 0,
      created_at TEXT    NOT NULL,
      updated_at TEXT    NOT NULL
    )
  """)

@flow Nav
  state :list
  state :editor
  state :reader
  state :search
  initial :list

  transition :to_editor  from :list      to :editor  watch: App is :editing _
  transition :to_reader  from :list      to :reader  watch: App is :reading _ or App is :new_note _
  transition :to_search  from :list      to :search  watch: App is :searching _
  transition :to_list    from :editor    to :list    watch: App is :list
  transition :to_list    from :reader    to :list    watch: App is :list
  transition :to_list    from :search    to :list    watch: App is :list
```

```deck
-- models/note.deck

@type Note
  id         : int
  title      : str
  content    : str
  tags       : [str]
  pinned     : bool
  created_at : str
  updated_at : str
  -- Computed fields (not persisted):
  word_count : int
  excerpt    : str
  has_meta   : bool
  meta_title : str?

fn from_row (row: {str: any}) -> Note =
  let content = unwrap_opt_or(row.str(row, "content"), "")
  Note {
    id:         unwrap_opt_or(row.int(row, "id"),           0),
    title:      unwrap_opt_or(row.str(row, "title"),        ""),
    content:    content,
    tags:       parse_tags(unwrap_opt_or(row.str(row, "tags"), "[]")),
    pinned:     unwrap_opt_or(row.bool(row, "pinned"),      false),
    created_at: unwrap_opt_or(row.str(row, "created_at"),   ""),
    updated_at: unwrap_opt_or(row.str(row, "updated_at"),   ""),
    word_count: md.word_count(content),
    excerpt:    md.excerpt(content, 160, "…"),
    has_meta:   md.has_front_matter(content),
    meta_title: extract_meta_title(content)
  }

fn save (note: Note) -> Result Note str !db =
  let now = time.to_iso(time.now())
  match note.id == 0
    | true ->
        db.exec("""
          INSERT INTO notes (title, content, tags, pinned, created_at, updated_at)
          VALUES (?, ?, ?, ?, ?, ?)
        """, [note.title, note.content, text.json(note.tags),
              note.pinned, now, now])
        |>? _ ->
          match db.query_one("SELECT last_insert_rowid() as id")
            | :err e -> :err e
            | :ok :none -> :err "Insert failed"
            | :ok :some row ->
                :ok note with {
                  id: unwrap_opt_or(row.int(row, "id"), 0),
                  created_at: now,
                  updated_at: now
                }
    | false ->
        db.exec("""
          UPDATE notes SET title=?, content=?, tags=?, pinned=?, updated_at=?
          WHERE id=?
        """, [note.title, note.content, text.json(note.tags),
              note.pinned, now, note.id])
        |> _ -> :ok note with { updated_at: now }

fn all () -> Result [Note] str !db =
  db.query("SELECT * FROM notes ORDER BY pinned DESC, updated_at DESC")
  |> map_ok(rows -> map(rows, from_row))

fn search (query: str) -> Result [Note] str !db =
  db.query("""
    SELECT * FROM notes
    WHERE content LIKE ? OR title LIKE ?
    ORDER BY updated_at DESC
  """, ["%{query}%", "%{query}%"])
  |> map_ok(rows -> map(rows, from_row))

fn delete (id: int) -> Result unit str !db =
  db.exec("DELETE FROM notes WHERE id = ?", [id])

@private
fn extract_meta_title (content: str) -> str? =
  match md.has_front_matter(content)
    | true  ->
        let meta = md.front_matter(content)
        row.str(meta, "title")
    | false -> :none
```

```deck
-- views/note_list_view.deck

@machine NoteListView
  state :idle
  state :loading
  state :loaded (notes: [Note])
  state :error  (message: str)
  initial :idle

  transition :load   from :idle from :error _  to :loading
  transition :loaded (notes: [Note])  from :loading  to :loaded (notes: notes)
  transition :failed (message: str)   from :loading  to :error (message: message)
  transition :reload  from *  to :loading

  on enter -> do  send(:load)  load_notes()

  body =
    screen
      header "Notes"
        actions
          button "⌕"
            -> App.send(:search)
            accessibility: "Search notes"
          button "✦"
            -> new_note()
            accessibility: "New note"
      match state
        | :idle | :loading -> center  spinner
        | :error s ->
            center
              status  icon: :error  message: s.message
              button "Retry"  -> do  send(:reload)  load_notes()
        | :loaded s ->
            when len(s.notes) == 0
              center
                text "No notes yet"  style: :muted
                text "Tap ✦ to create your first note"  style: :muted :small
            list s.notes
              item n ->
                note_card(n)
              on refresh -> do  send(:reload)  load_notes()

fn note_card (n: Note) =
  card
    row
      column
        row
          when n.pinned  text "📌"  style: :small
          text (unwrap_opt_or(n.meta_title, n.title))  style: :heading
        text "{md.reading_time(n.content) |> time.duration_str} read · {n.word_count} words"
          style: :caption :muted
        text n.excerpt  style: :muted :small
        when len(n.tags) > 0
          row
            for tag in n.tags
              text "#{tag}"  style: :caption :muted
    actions
      button "Edit"
        -> App.send(:edit_note, note: n)
        style: :ghost
      button "Open"
        -> App.send(:open_note, note: n)
        style: :ghost

fn load_notes () -> unit !db =
  match note.all()
    | :err e -> send(:failed, message: e)
    | :ok ns -> send(:loaded, notes: ns)

fn new_note () -> unit !db =
  let n = Note {
    id: 0, title: "New Note", content: "", tags: [], pinned: false,
    created_at: "", updated_at: "", word_count: 0,
    excerpt: "", has_meta: false, meta_title: :none
  }
  App.send(:edit_note, note: n)
```

```deck
-- views/note_editor_view.deck

@machine NoteEditorView
  param note : Note

  state :editing  (current: Note, saved: bool, editor: MdEditorState?)
  initial :editing (current: note, saved: true, editor: :none)

  transition :update_content (text: str)
    from :editing s
    to   :editing (
      current: s.current with {
        title:      extract_title(text),
        content:    text,
        word_count: md.word_count(text),
        excerpt:    md.excerpt(text, 160, "…")
      },
      saved:  false,
      editor: :none
    )

  transition :update_editor (editor: MdEditorState)
    from :editing s
    to   :editing (
      current: s.current with {
        content:    editor.content,
        title:      extract_title(editor.content),
        word_count: md.word_count(editor.content),
        excerpt:    md.excerpt(editor.content, 160, "…")
      },
      saved:  false,
      editor: :some editor
    )

  transition :saved (note: Note)
    from :editing s
    to   :editing (current: note, saved: true, editor: s.editor)

  body =
    screen
      match state
        | :editing s ->
            column
              row
                text s.current.title  style: :heading
                spacer
                text (match s.saved | true -> "Saved" | false -> "Editing")  style: :muted :small
              text "{s.current.word_count} words · {time.duration_str(md.reading_time(s.current.content))}"
                style: :caption :muted
              divider
              match config.preview_on_edit
                | false ->
                    markdown_editor
                      value:    s.current.content
                      on change -> send(:update_content, text: event.value)
                      toolbar:    true
                      min_lines:  20
                | true ->
                    markdown_editor
                      value:       s.current.content
                      on change -> send(:update_content, text: event.value)
                      on cursor -> send(:update_editor,
                                        editor: md_cap.editor_set_cursor(
                                          unwrap_opt_or(s.editor, md_cap.editor_new(s.current.content)),
                                          event.cursor))
                      editor_state: s.editor
                      toolbar:     true
                      preview:     :side
                      min_lines:   20
              row
                button "Save"
                  -> do_save(s.current)
                  style: :primary
                  enabled: not s.saved
                button "Discard"
                  confirm:       "Discard changes?"
                  confirm_label: "Discard"
                  on confirm -> App.send(:discard)
                  style: :ghost

fn do_save (n: Note) -> unit !db =
  match note.save(n)
    | :err _ -> unit
    | :ok saved -> do
        send(:saved, note: saved)
        App.send(:save, note: saved)

fn extract_title (content: str) -> str =
  let headings = md.headings(content)
  match head(headings)
    | :some h -> h.text
    | :none   ->
        let plain = md.to_plain(content)
        let first_line = head(text.lines(plain))
        match first_line
          | :some l -> text.truncate(l, 60)
          | :none   -> "Untitled"
```

```deck
-- views/note_reader_view.deck

@machine NoteReaderView
  param note : Note

  state :reading (scroll_to: str?)
  initial :reading (scroll_to: :none)

  transition :jump_to (id: str)
    from *
    to   :reading (scroll_to: :some id)

  on enter -> unit

  body =
    screen
      match state
        | :reading s ->
            column
              row
                text (unwrap_opt_or(note.meta_title, note.title))  style: :large :heading
                spacer
                text time_ago.format(note.updated_at)  style: :muted :small
              text "{note.word_count} words · {time.duration_str(md.reading_time(note.content))}"
                style: :caption :muted
              when len(md.headings(note.content)) > 3
                -- Table of contents for longer docs
                card
                  text "Contents"  style: :subheading
                  for h in md.headings(note.content)
                    button (text.repeat("  ", h.level - 1) ++ h.text)
                      -> send(:jump_to, id: h.id)
                      style: :ghost
              divider
              markdown note.content
                style:     config.default_style
                show_toc:  false
                scroll_to: s.scroll_to
                code_copy: true
                on link ->
                  match event.url |> text.starts("note://")
                    | true  ->
                        let target_id = text.slice(event.url, 7, text.length(event.url))
                        send(:jump_to, id: target_id)
                    | false -> unit
              spacer
              actions
                button "Edit"
                  -> App.send(:edit_note, note: note)
                  style: :primary
                button "Back"
                  -> App.send(:back)
                  style: :ghost
```

### 7.2 E-Reader

```deck
-- app.deck (e-reader)

@app
  name:    "Read"
  id:      "mx.lab.reader"
  version: "1.0.0"

@use
  fs              as fs
  db              as db
  network.http    as http     when: network is :connected
  display.screen  as screen
  display.backlight as backlight
  markdown        as md_cap
  ./models/book
  ./views/library
  ./views/reader

@config
  font_size     : int   = 18  range: 12..28  unit: "pt"
  line_height   : float = 1.7 range: 1.2..2.2
  margin        : int   = 24  range: 8..48   unit: "px"
  theme         : atom  = :auto  options: [:light, :dark, :sepia, :auto]
  auto_dimming  : bool  = true

@machine Reader
  state :closed
  state :open (book: Book, position: int, chapter_id: str?)
  state :searching (book: Book, query: str, results: [MdHeading])

  initial :closed

  transition :open_book (book: Book)
    from :closed
    to   :open (book: book, position: 0, chapter_id: :none)

  transition :restore (book: Book, position: int, chapter_id: str?)
    from :closed
    to   :open (book: book, position: position, chapter_id: chapter_id)

  transition :jump (heading_id: str)
    from :open s
    to   :open (book: s.book, position: s.position, chapter_id: :some heading_id)

  transition :search (query: str)
    from :open s
    to   :searching (book: s.book, query: query, results: [])

  transition :search_done (results: [MdHeading])
    from :searching s
    to   :searching (book: s.book, query: s.query, results: results)

  transition :close_search  from :searching s  to :open (book: s.book, position: 0, chapter_id: :none)
  transition :close         from *             to :closed

@flow App
  state :library
  state :reading
  initial :library

  transition :open   from :library  to :reading  watch: Reader is :open or Reader is :searching
  transition :close  from :reading  to :library  watch: Reader is :closed

@on hardware.button (id: 0, action: :press)
  match Reader.state
    | :open _ -> display.backlight_toggle()
    | _       -> unit
```

```deck
-- views/reader_view.deck

@machine ReaderView

  state :idle
  state :loading (path: str)
  state :ready   (content: str, doc: MdDocument)
  state :error   (message: str)
  initial :idle

  transition :load (path: str)  from :idle from :error _  to :loading (path: path)
  transition :ready (content: str, doc: MdDocument)
    from :loading _
    to   :ready (content: content, doc: doc)
  transition :failed (message: str)
    from :loading _
    to   :error (message: message)

  on enter ->
    match Reader.state
      | :open s      -> do  send(:load, path: s.book.path)
      | :searching s -> do  send(:load, path: s.book.path)
      | _            -> unit

  body =
    screen
      match state
        | :idle | :loading _ -> center  spinner
        | :error s           -> status  icon: :error  message: s.message
        | :ready s ->
            match Reader.state
              | :open r ->
                  reader_body(s.doc, r.chapter_id, build_theme())
              | :searching r ->
                  column
                    input
                      value:       r.query
                      placeholder: "Search in book…"
                      style:       :search
                      on change -> Reader.send(:search, query: event.value)
                    when len(r.results) > 0
                      list r.results
                        item h ->
                          card
                            text "H{h.level}: {h.text}"
                            button "Jump"
                              -> Reader.send(:jump, heading_id: h.id)
                              style: :ghost
                    button "Close search"
                      -> Reader.send(:close_search)
                      style: :ghost
              | _ -> unit

fn reader_body (doc: MdDocument, chapter_id: str?, theme: MdTheme) =
  scroll
    column
      markdown doc
        style:      :prose
        theme:      theme
        show_toc:   true
        scroll_to:  chapter_id
        selectable: true
        code_copy:  true
        virtual:    true
        on link -> handle_link(event.url)
        on image -> unit

fn build_theme () -> MdTheme =
  let base_scale = float(config.font_size) / 16.0
  MdTheme {
    body_font:      :none,
    code_font:      :none,
    heading_scale:  :some base_scale,
    line_height:    :some config.line_height,
    code_bg:        :none,
    link_color:     :none,
    blockquote_bar: :none,
    max_width:      :some 680
  }

fn handle_link (url: str) -> unit =
  match text.starts(url, "#")
    | true  ->
        let id = text.slice(url, 1, text.length(url))
        Reader.send(:jump, heading_id: id)
    | false -> unit

fn load_book (path: str) -> unit !fs =
  match fs.read(path)
    | :err e -> send(:failed, message: "Cannot open file: {e}")
    | :ok content ->
        let doc = md.parse(content, { "front_matter": true, "heading_ids": true })
        send(:ready, content: content, doc: doc)
```

### 7.3 Markdown in Chat (AI Responses)

```deck
-- views/chat_view.deck

@machine ChatView

  state :idle
  state :waiting (messages: [ChatMsg], partial: str)

  initial :idle

  transition :user_sent (msg: str)
    from :idle
    to   :waiting (messages: [ChatMsg { role: :user, content: msg }], partial: "")

  transition :token (text: str)
    from :waiting s
    to   :waiting (messages: s.messages, partial: "{s.partial}{text}")

  transition :response_done
    from :waiting s
    to   :idle with messages: s.messages ++ [ChatMsg { role: :assistant, content: s.partial }]

  body =
    screen
      scroll
        match state
          | :idle s -> message_list(s.messages)
          | :waiting s ->
              column
                message_list(s.messages)
                -- Streaming response with live markdown render
                card
                  markdown s.partial
                    style:   :compact
                    virtual: false
                    -- As partial grows, component re-renders reactively

      row
        input
          value:       compose_text
          placeholder: "Message…"
          on change -> update_compose(event.value)
        button "Send"
          -> do_send()
          enabled: not text.is_blank(compose_text)
          style: :primary

fn message_list (msgs: [ChatMsg]) =
  column
    for msg in msgs
      card
        match msg.role
          | :user ->
              row
                spacer
                text msg.content  style: :body
          | :assistant ->
              row
                column
                  markdown msg.content
                    style:     :compact
                    code_copy: true
                spacer
```

### 7.4 Technical Documentation Browser

```deck
-- views/docs_view.deck

@machine DocsView
  param source : str      -- markdown content

  state :navigating (scroll_to: str?)
  initial :navigating (scroll_to: :none)

  transition :section (id: str)
    from *
    to   :navigating (scroll_to: :some id)

  body =
    screen
      let doc = md.parse(source, { "heading_ids": true })
      row
        -- Sidebar ToC (wider screens via adaptive layout hint)
        when len(doc.toc) > 0
          column
            text "Contents"  style: :subheading
            for h in doc.toc
              button h.text
                -> send(:section, id: h.id)
                style: :ghost
        -- Content
        scroll
          match state
            | :navigating s ->
                column
                  markdown doc
                    style:     :normal
                    scroll_to: s.scroll_to
                    code_copy: true
                    show_toc:  false
                    virtual:   true
                    on link -> handle_doc_link(event.url, event.text)

fn handle_doc_link (url: str, label: str) -> unit =
  match text.starts(url, "#")
    | true  ->
        let id = text.slice(url, 1, text.length(url))
        send(:section, id: id)
    | false -> notify.send("External link: {label}")
```

---

## 8. Tagged String Literal (Tooling Hint)

Deck code can use `md"""..."""` as a tagged multi-line string. Semantically identical to `"""..."""` — same type, same runtime value. The tag is a hint to editors and language servers to apply Markdown syntax highlighting inside the string:

```deck
let post_body = md"""
# Getting Started

Install the interpreter:

```sh
deck run app.deck
```

Then create your first app.
"""

-- Identical at runtime to:
let post_body = """
# Getting Started

Install the interpreter:
...
"""
```

**Implementation in the lexer:** `md"""` is recognized as `TOK_MD_MULTILINE_STR`, treated identically to `TOK_MULTILINE_STR` during parsing and evaluation. The tag is stripped before the string value is produced. No behavior change — only tooling metadata.

---

## 9. Performance Notes

### 9.1 Parse Once, Render Many

For content shown in multiple places or views, parse once and pass `MdDocument`:

```deck
-- In @on launch or a task:
let rendered_readme = md.parse(fs.read("README.md"))
-- Store in machine state or cache

-- In views: pass the pre-parsed document
markdown rendered_readme
```

Parsing is O(n) in source length. Rendering the component tree is O(k) in node count. For a 10,000-word document (~60KB), parse takes ~2ms on a 240MHz ESP32; rendering the full component tree takes ~5ms. Virtual rendering reduces the component tree to ~30 nodes regardless of document size.

### 9.2 Virtual Rendering

Auto-enabled for documents over 10,000 characters. Renders only nodes within `~2× viewport height`. As the user scrolls, nodes are re-rendered on demand. The `MdDocument` AST is always fully parsed and resident; only the `ComponentNode` tree is virtualized.

### 9.3 Excerpt for List Views

Never render full markdown in a list of notes/posts. Use `md.excerpt()` for list cards and only render the full `markdown` component in the reader view:

```deck
-- List view (fast):
text md.excerpt(note.content, 120, "…")  style: :muted :small

-- Reader view (full):
markdown note.content  style: :prose
```

### 9.4 Front Matter Parsing

`md.front_matter()` parses the entire document to extract metadata. If you call it alongside `md.parse()`, pass the already-parsed `MdDocument` to avoid double-parse:

```deck
-- Inefficient:
let meta = md.front_matter(content)
let doc  = md.parse(content)

-- Efficient: parse once, extract from doc
let doc  = md.parse(content, { "front_matter": true })
let meta = map.get(doc, "front_matter")  -- stored in doc when option enabled
```

When `front_matter: true` is passed to `md.parse`, the parsed metadata map is available as `doc.front_matter` (an extra field on `MdDocument`, `{str: any}?`).

---

## 10. Startup Integration

```c
/* In main application before deck_runtime_start() */

DeckRuntime* rt = deck_runtime_create(&cfg);

/* Register the markdown module */
markdown_module_register(rt);

/* Register everything else */
bmp280_register(rt);

deck_runtime_load(rt);
deck_runtime_start(rt);
```

The module is self-contained. If md4c is not linked, the linker will report missing symbols. There are no runtime checks for presence — the module is either compiled in or not.
