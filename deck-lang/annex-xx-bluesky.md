# Annex XX — Bluesky ATProto Client
**Kitchen-Sink Reference App**

> **What this annex is.** Bluesky is a deliberately large, realistic Deck app — the closest thing the project has to a stress test. It exercises almost every language feature and OS capability simultaneously: authentication state machines, paginated lists, infinite scroll, real-time-feeling UI from polled data, background token refresh, deep links, signed/unsigned posts, image rendering, time formatting, error recovery, offline cache, the lot. If a refactor of the language or runtime breaks Bluesky, it almost certainly breaks something in production.
>
> **It is not the next app on the build roadmap.** The bundled apps (Annexes A–D: Launcher, Task Manager, Settings, Files) come first. Several other apps not yet specified (catalogued in `APPS.md`) come after. Bluesky sits at the end of the alphabet by intent — it is "Annex XX" with no fixed slot. When ready, it will install on SD as `social.bsky.app`, signed by the community key, like any other third-party app.
>
> **Use this annex as a syntax reference.** When you want to see how a real Deck app structures its `@app`, `@requires`, `@machine`, `@flow`, `@stream`, `@task`, `@migration`, `@on`, content bodies, and capability use across many `.deck` files, read this annex end-to-end. The patterns here are the load-bearing examples the docs site links to.

---

## Overview

A functionally complete, lite Bluesky client for the AT Protocol. Feature scope:
- Login / logout / session persistence and token auto-refresh
- Home timeline (following feed) with pagination
- Notifications with unread badge
- Create post, reply, delete post
- Like / unlike, repost / unrepost
- Follow / unfollow
- Profile view (own + others)
- Post thread / detail with replies
- Search (posts and users)
- Settings

Out of scope: image upload, video, custom feeds, lists, labelers.

---

## File Structure

```
bluesky/
  app.deck
  types.deck
  api/
    auth.deck
    feed.deck
    actor.deck
    interaction.deck
    notification.deck
  utils/
    xrpc.deck
    time_ago.deck
  flows/
    app.deck
    main.deck
    login.deck
    timeline.deck
    notifications.deck
    profile.deck
    compose.deck
    search.deck
    settings.deck
  tasks/
    token_refresh.deck
    timeline_refresh.deck
    notif_refresh.deck
```

---

## types.deck

```deck
@doc "AT Protocol domain types."

@type Session
  did         : str
  handle      : str
  access_jwt  : str
  refresh_jwt : str

@type Author
  did          : str
  handle       : str
  display_name : str?
  avatar       : str?

@type Post
  uri         : str
  cid         : str
  author      : Author
  text        : str
  created_at  : str
  like_count  : int
  reply_count : int
  repost_count: int
  liked_by_me    : bool
  reposted_by_me : bool
  like_rkey   : str?
  repost_rkey : str?
  reply_ref   : ReplyRef?

@type ReplyRef
  root_uri    : str
  root_cid    : str
  parent_uri  : str
  parent_cid  : str

@type Thread
  post    : Post
  replies : [Thread]

@type Profile
  did           : str
  handle        : str
  display_name  : str?
  bio           : str?
  avatar        : str?
  followers     : int
  following     : int
  posts_count   : int
  is_following  : bool
  follow_rkey   : str?

@type Notif
  reason    : str
  author    : Author
  is_read   : bool
  indexed_at: str

@type FeedPage
  posts  : [Post]
  cursor : str?

@type SearchPage
  posts  : [Post]
  users  : [Profile]
  cursor : str?
```

---

## app.deck

```deck
@app
  name:    "Bluesky"
  id:      "social.bsky.app"
  version: "1.0.0"

@use
  network.http    as http      when: network is :connected
  storage.local   as store
  nvs             as nvs
  cache           as cache
  display.notify  as notify
  api_client      as api
  system.info     as sysinfo
  ./types
  ./utils/xrpc
  ./utils/time_ago
  ./api/auth
  ./api/feed
  ./api/actor
  ./api/interaction
  ./api/notification
  ./flows/app
  ./flows/main
  ./flows/login
  ./flows/timeline
  ./flows/notifications
  ./flows/profile
  ./flows/compose
  ./flows/search
  ./flows/settings
  ./tasks/token_refresh
  ./tasks/timeline_refresh
  ./tasks/notif_refresh
  -- math, text, time are builtins — always in scope, no @use needed

@permissions
  network.http    reason: "Connect to Bluesky to fetch your timeline and notifications"
  storage.local   reason: "Cache timeline and session data between sessions"
  display.notify  reason: "Alert you to new notifications"

@config
  bsky_host     : str  = "https://bsky.social"
  page_size     : int  = 30   range: 10..50
  notif_interval: int  = 60   range: 15..300   unit: "seconds"
  notifications : bool = true
  show_reposts  : bool = true

-- ── Auth Machine ─────────────────────────────────────────────────────────────

@machine Auth
  state :unauthenticated (error: str?)
  state :authenticating
  state :authenticated   (did: str, handle: str)
  state :refreshing      (did: str, handle: str)

  initial :unauthenticated (error: :none)

  transition :attempt
    from :unauthenticated _
    to   :authenticating

  transition :success (did: str, handle: str)
    from :authenticating
    to   :authenticated (did: did, handle: handle)

  transition :failed (error: str)
    from :authenticating
    to   :unauthenticated (error: :some error)

  transition :refresh_start
    from :authenticated s
    to   :refreshing (did: s.did, handle: s.handle)

  transition :refresh_ok
    from :refreshing s
    to   :authenticated (did: s.did, handle: s.handle)

  transition :refresh_fail
    from :refreshing _
    to   :unauthenticated (error: :some "Session expired. Please sign in again.")

  transition :logout
    from *
    to   :unauthenticated (error: :none)

-- ── Timeline Machine ─────────────────────────────────────────────────────────

@machine Timeline
  state :empty
  state :loading
  state :loaded     (posts: [Post], cursor: str?)
  state :refreshing (posts: [Post])
  state :paginating (posts: [Post], cursor: str)
  state :error      (message: str)

  initial :empty

  transition :load
    from :empty
    from :error _
    to   :loading

  transition :loaded (posts: [Post], cursor: str?)
    from :loading
    to   :loaded (posts: posts, cursor: cursor)

  transition :failed (message: str)
    from :loading
    to   :error (message: message)

  transition :refresh
    from :loaded s
    to   :refreshing (posts: s.posts)

  transition :refreshed (posts: [Post], cursor: str?)
    from :refreshing _
    to   :loaded (posts: posts, cursor: cursor)

  transition :paginate
    from :loaded s
    when: s.cursor is :some
    to   :paginating (posts: s.posts, cursor: unwrap_opt(s.cursor))

  transition :paginated (new_posts: [Post], cursor: str?)
    from :paginating s
    to   :loaded (posts: append_all(s.posts, new_posts), cursor: cursor)

-- ── Notifications Machine ────────────────────────────────────────────────────

@machine Notifs
  state :empty
  state :loading
  state :loaded (items: [Notif], unread: int)
  state :error  (message: str)

  initial :empty

  transition :load    from :empty from :error _  to :loading
  transition :loaded (items: [Notif], unread: int)
    from :loading
    to   :loaded (items: items, unread: unread)
  transition :failed (message: str)
    from :loading
    to   :error (message: message)
  transition :refreshed (items: [Notif], unread: int)
    from :loaded _
    to   :loaded (items: items, unread: unread)
  transition :marked_read
    from :loaded s
    to   :loaded (items: s.items, unread: 0)

-- ── Compose Machine ──────────────────────────────────────────────────────────

@machine Compose
  state :closed
  state :open    (text: str, reply_to: Post?)
  state :posting (text: str)
  state :error   (text: str, message: str)

  initial :closed

  transition :open_new
    from :closed
    to   :open (text: "", reply_to: :none)

  transition :open_reply (post: Post)
    from :closed
    to   :open (text: "", reply_to: :some post)

  transition :update (text: str)
    from :open s
    to   :open (text: text, reply_to: s.reply_to)

  transition :submit
    from :open s
    when: text.length(s.text) > 0 and text.length(s.text) <= 300
    to   :posting (text: s.text)

  transition :done   from :posting _  to :closed
  transition :failed (message: str)
    from :posting s
    to   :error (text: s.text, message: message)
  transition :retry
    from :error s
    to   :posting (text: s.text)
  transition :cancel  from *  to :closed

-- ── Streams ──────────────────────────────────────────────────────────────────

-- UnreadCount is read directly from Notifs.state in content bodies.
-- No @stream needed: if a content= body reads from a machine, the runtime
-- re-evaluates automatically when the machine transitions (02-deck-app §8.7).

-- ── Lifecycle ────────────────────────────────────────────────────────────────

@on launch
  api.configure(ApiConfig {
    base_url:    "{config.bsky_host}/xrpc",
    timeout:     :some 15s,
    cache_ttl:   :some 30s,
    retry_count: :some 2,
    retry_on:    :some [:timeout, :server_error],
    user_agent:  :some "Bluesky-Deck/1.0"
  })
  match auth.load_session()
    | :none -> unit
    | :some s ->
        api.set_token(s.access_jwt)
        Auth.send(:success, did: s.did, handle: s.handle)
        Timeline.send(:load)
        Notifs.send(:load)

@on resume
  match Auth.state
    | :authenticated _ -> Timeline.send(:refresh)
    | _                -> unit

-- ── Navigation ───────────────────────────────────────────────────────────────

@flow App
  state :login     flow: LoginFlow
  state :main      flow: MainFlow
  state :compose   flow: ComposeFlow
  state :settings  flow: SettingsFlow

  initial :login

  transition :authenticated
    from :login
    to   :main
    watch: Auth is :authenticated

  transition :logged_out
    from :main
    to   :login
    watch: Auth is :unauthenticated

  transition :open_compose
    from :main
    to   :compose

  transition :close_compose
    from :compose
    to   history

  transition :open_settings
    from :main
    to   :settings

  transition :close_settings
    from :settings
    to   history


@flow MainFlow
  state :timeline      flow: TimelineFlow
  state :notifications flow: NotificationsFlow
  state :profile       flow: ProfileFlow
  state :search        flow: SearchFlow

  initial :timeline

  transition :go_timeline      from * to :timeline
  transition :go_notifications from * to :notifications
  transition :go_profile       from * to :profile
  transition :go_search        from * to :search
```

---

## utils/xrpc.deck

```deck
@doc "AT Protocol XRPC helpers."

fn get (nsid: str, params: {str: str}) -> Result {str: any} str !api =
  let path = "{nsid}?{text.query_build(params)}"
  match api.get(path)
    | :err e -> :err (map_api_error(e))
    | :ok r  ->
        match r.json
          | :none   -> :err "Invalid JSON response"
          | :some j -> check_error(j)

fn post_rpc (nsid: str, body: {str: any}) -> Result {str: any} str !api =
  match api.post(nsid, body)
    | :err e -> :err (map_api_error(e))
    | :ok r  ->
        match r.json
          | :none   -> :err "Invalid JSON response"
          | :some j -> check_error(j)

@private
fn check_error (body: {str: any}) -> Result {str: any} str =
  match map.get(body, "error")
    | :some code ->
        let msg = unwrap_opt_or(map.get(body, "message") |>? is_str, code)
        :err msg
    | :none -> :ok body

@private
fn map_api_error (e: atom) -> str =
  match e
    | :offline       -> "No internet connection"
    | :unauthorized  -> "Session expired — please sign in again"
    | :timeout       -> "Request timed out"
    | :not_found     -> "Not found"
    | :rate_limited  -> "Too many requests — please wait"
    | :server_error  -> "Bluesky server error"
    | _              -> "Network error"

@private
fn is_str (v: any) -> str? =
  match v is str
    | true  -> :some v
    | false -> :none
```

---

## utils/time_ago.deck

```deck
fn format (iso: str) -> str =
  match time.from_iso(iso)
    | :none -> iso
    | :some t ->
        let parts = time.duration_parts(time.since(t))
        match parts
          | p when p.seconds < 60 -> "now"
          | p when p.minutes < 60 -> "{p.minutes}m"
          | p when p.hours < 24   -> "{p.hours}h"
          | p when p.days < 7     -> "{p.days}d"
          | _                     -> time.format(t, "MMM d")
```

---

## api/auth.deck

```deck
fn login (identifier: str, password: str) -> Result Session str !api !nvs =
  Auth.send(:attempt)
  match xrpc.post_rpc("com.atproto.server.createSession", {
    "identifier": identifier,
    "password":   password
  })
    | :err e -> do
        Auth.send(:failed, error: e)
        :err e
    | :ok body ->
        match parse_session(body)
          | :none -> :err "Invalid session response"
          | :some s ->
              save_session(s)
              api.set_token(s.access_jwt)
              Auth.send(:success, did: s.did, handle: s.handle)
              :ok s

fn logout () -> unit !api !nvs =
  xrpc.post_rpc("com.atproto.server.deleteSession", {})
  api.clear_token()
  nvs.delete("bsky_session")
  Auth.send(:logout)

fn refresh () -> Result unit str !api !nvs =
  match load_session()
    | :none -> :err "No session"
    | :some s ->
        api.set_token(s.refresh_jwt)
        match xrpc.post_rpc("com.atproto.server.refreshSession", {})
          | :err _ ->
              Auth.send(:refresh_fail)
              :err "Refresh failed"
          | :ok body ->
              match parse_session(body)
                | :none -> :err "Invalid refresh response"
                | :some ns ->
                    save_session(ns)
                    api.set_token(ns.access_jwt)
                    Auth.send(:refresh_ok)
                    :ok unit

fn load_session () -> Session? =
  match nvs.get("bsky_session")
    | :none -> :none
    | :some raw ->
        match text.from_json(raw)
          | :none -> :none
          | :some j -> parse_session(j)

@private
fn save_session (s: Session) -> unit !nvs =
  nvs.set("bsky_session", text.json({
    "did":         s.did,
    "handle":      s.handle,
    "accessJwt":   s.access_jwt,
    "refreshJwt":  s.refresh_jwt
  }))

@private
fn parse_session (body: {str: any}) -> Session? =
  let d = map.get(body, "did")
  let h = map.get(body, "handle")
  let a = map.get(body, "accessJwt")
  let r = map.get(body, "refreshJwt")
  match (d, h, a, r)
    | (:some did, :some handle, :some access, :some refresh)
        when is_str(did) and is_str(handle) and is_str(access) and is_str(refresh) ->
        :some Session {
          did:         did,
          handle:      handle,
          access_jwt:  access,
          refresh_jwt: refresh
        }
    | _ -> :none
```

---

## api/feed.deck

```deck
fn get_timeline (cursor: str?) -> Result FeedPage str !api =
  let params = base_params(cursor)
  match xrpc.get("app.bsky.feed.getTimeline", params)
    | :err e -> :err e
    | :ok body -> :ok parse_feed_page(body)

fn get_author_feed (did: str, cursor: str?) -> Result FeedPage str !api =
  let params = map.set(base_params(cursor), "actor", did)
  match xrpc.get("app.bsky.feed.getAuthorFeed", params)
    | :err e -> :err e
    | :ok body -> :ok parse_feed_page(body)

fn get_thread (uri: str) -> Result Thread str !api =
  match xrpc.get("app.bsky.feed.getPostThread", { "uri": uri, "depth": "6" })
    | :err e -> :err e
    | :ok body ->
        match map.get(body, "thread")
          | :none -> :err "Thread not found"
          | :some t -> :ok parse_thread(t)

fn search_posts (q: str, cursor: str?) -> Result FeedPage str !api =
  let params = map.set(base_params(cursor), "q", q)
  match xrpc.get("app.bsky.feed.searchPosts", params)
    | :err e -> :err e
    | :ok body ->
        let posts  = body |> get_list("posts") |> map(parse_post_view)
        let cursor = str_field(body, "cursor")
        :ok FeedPage { posts: posts, cursor: cursor }

@private
fn base_params (cursor: str?) -> {str: str} =
  let base = { "limit": str(config.page_size) }
  match cursor
    | :none   -> base
    | :some c -> map.set(base, "cursor", c)

@private
fn parse_feed_page (body: {str: any}) -> FeedPage =
  let raw_feed = get_list(body, "feed")
  let posts    = raw_feed
    |> filter(p -> config.show_reposts or not is_repost(p))
    |> map(parse_feed_view_post)
  FeedPage {
    posts:  posts,
    cursor: str_field(body, "cursor")
  }

@private
fn parse_feed_view_post (item: {str: any}) -> Post =
  match map.get(item, "post")
    | :none    -> empty_post()
    | :some p  -> parse_post_view(p)

@private
fn parse_post_view (p: {str: any}) -> Post =
  let record   = unwrap_opt_or(map.get(p, "record"), {})
  let viewer   = unwrap_opt_or(map.get(p, "viewer"), {})
  let like_uri = str_field(viewer, "like")
  let rp_uri   = str_field(viewer, "repost")
  Post {
    uri:          str_field_req(p, "uri"),
    cid:          str_field_req(p, "cid"),
    author:       parse_author(unwrap_opt_or(map.get(p, "author"), {})),
    text:         str_field_req(record, "text"),
    created_at:   str_field_req(record, "createdAt"),
    like_count:   int_field(p, "likeCount"),
    reply_count:  int_field(p, "replyCount"),
    repost_count: int_field(p, "repostCount"),
    liked_by_me:  is_some(like_uri),
    reposted_by_me:  is_some(rp_uri),
    like_rkey:    like_uri |> map_opt(extract_rkey),
    repost_rkey:  rp_uri   |> map_opt(extract_rkey),
    reply_ref:    parse_reply_ref(record)
  }

@private
fn parse_thread (node: {str: any}) -> Thread =
  let post    = parse_post_view(unwrap_opt_or(map.get(node, "post"), {}))
  let replies = get_list(node, "replies") |> map(parse_thread)
  Thread { post: post, replies: replies }

@private
fn parse_author (a: {str: any}) -> Author =
  Author {
    did:          str_field_req(a, "did"),
    handle:       str_field_req(a, "handle"),
    display_name: str_field(a, "displayName"),
    avatar:       str_field(a, "avatar")
  }

@private
fn parse_reply_ref (record: {str: any}) -> ReplyRef? =
  match map.get(record, "reply")
    | :none -> :none
    | :some r ->
        let root   = unwrap_opt_or(map.get(r, "root"),   {})
        let parent = unwrap_opt_or(map.get(r, "parent"), {})
        :some ReplyRef {
          root_uri:   str_field_req(root,   "uri"),
          root_cid:   str_field_req(root,   "cid"),
          parent_uri: str_field_req(parent, "uri"),
          parent_cid: str_field_req(parent, "cid")
        }

@private
fn is_repost (item: {str: any}) -> bool = map.has(item, "reason")

@private
fn empty_post () -> Post =
  Post {
    uri: "", cid: "", text: "", created_at: "",
    author: Author { did: "", handle: "", display_name: :none, avatar: :none },
    like_count: 0, reply_count: 0, repost_count: 0,
    liked_by_me: false, reposted_by_me: false,
    like_rkey: :none, repost_rkey: :none, reply_ref: :none
  }

-- Field helpers
@private
fn str_field     (m: {str: any}, k: str) -> str? = row.str(m, k)
@private
fn str_field_req (m: {str: any}, k: str) -> str  = unwrap_opt_or(row.str(m, k), "")
@private
fn int_field     (m: {str: any}, k: str) -> int  = unwrap_opt_or(row.int(m, k), 0)
@private
fn get_list      (m: {str: any}, k: str) -> [any] =
  match map.get(m, k)
    | :some v when is_list(v) -> v
    | _                       -> []
@private
fn extract_rkey (uri: str) -> str =
  let parts = text.split(uri, "/")
  unwrap_opt_or(last(parts), "")
```

---

## api/actor.deck

```deck
fn get_profile (actor: str) -> Result Profile str !api =
  match xrpc.get("app.bsky.actor.getProfile", { "actor": actor })
    | :err e -> :err e
    | :ok body -> :ok parse_profile(body)

fn search_actors (q: str) -> Result [Profile] str !api =
  match xrpc.get("app.bsky.actor.searchActors", { "q": q, "limit": "20" })
    | :err e -> :err e
    | :ok body ->
        :ok (get_list(body, "actors") |> map(parse_profile))

fn parse_profile (p: {str: any}) -> Profile =
  let viewer     = unwrap_opt_or(map.get(p, "viewer"), {})
  let follow_uri = row.str(viewer, "following")
  Profile {
    did:          str_req(p, "did"),
    handle:       str_req(p, "handle"),
    display_name: row.str(p, "displayName"),
    bio:          row.str(p, "description"),
    avatar:       row.str(p, "avatar"),
    followers:    int_f(p, "followersCount"),
    following:    int_f(p, "followsCount"),
    posts_count:  int_f(p, "postsCount"),
    is_following: is_some(follow_uri),
    follow_rkey:  follow_uri |> map_opt(extract_rkey)
  }

@private
fn str_req (m: {str: any}, k: str) -> str  = unwrap_opt_or(row.str(m, k), "")
@private
fn int_f   (m: {str: any}, k: str) -> int  = unwrap_opt_or(row.int(m, k), 0)
@private
fn extract_rkey (uri: str) -> str =
  let parts = text.split(uri, "/")
  unwrap_opt_or(last(parts), "")
```

---

## api/interaction.deck

```deck
fn like (uri: str, cid: str) -> Result str str !api =
  with_did(did ->
    match xrpc.post_rpc("com.atproto.repo.createRecord", {
      "repo":       did,
      "collection": "app.bsky.feed.like",
      "record": {
        "$type":     "app.bsky.feed.like",
        "subject":   { "uri": uri, "cid": cid },
        "createdAt": time.to_iso(time.now())
      }
    })
      | :err e   -> :err e
      | :ok body -> :ok (unwrap_opt_or(row.str(body, "uri"), ""))
  )

fn unlike (rkey: str) -> Result unit str !api =
  delete_record("app.bsky.feed.like", rkey)

fn repost (uri: str, cid: str) -> Result str str !api =
  with_did(did ->
    match xrpc.post_rpc("com.atproto.repo.createRecord", {
      "repo":       did,
      "collection": "app.bsky.feed.repost",
      "record": {
        "$type":     "app.bsky.feed.repost",
        "subject":   { "uri": uri, "cid": cid },
        "createdAt": time.to_iso(time.now())
      }
    })
      | :err e   -> :err e
      | :ok body -> :ok (unwrap_opt_or(row.str(body, "uri"), ""))
  )

fn unrepost (rkey: str) -> Result unit str !api =
  delete_record("app.bsky.feed.repost", rkey)

fn follow (did: str) -> Result str str !api =
  with_did(me ->
    match xrpc.post_rpc("com.atproto.repo.createRecord", {
      "repo":       me,
      "collection": "app.bsky.graph.follow",
      "record": {
        "$type":     "app.bsky.graph.follow",
        "subject":   did,
        "createdAt": time.to_iso(time.now())
      }
    })
      | :err e   -> :err e
      | :ok body -> :ok (unwrap_opt_or(row.str(body, "uri"), ""))
  )

fn unfollow (rkey: str) -> Result unit str !api =
  delete_record("app.bsky.graph.follow", rkey)

fn create_post (text_content: str, reply_to: Post?) -> Result unit str !api =
  with_did(did ->
    let record = match reply_to
      | :none -> {
          "$type":     "app.bsky.feed.post",
          "text":      text_content,
          "createdAt": time.to_iso(time.now())
        }
      | :some parent ->
          let ref  = unwrap_opt_or(parent.reply_ref, ReplyRef {
            root_uri:   parent.uri, root_cid:   parent.cid,
            parent_uri: parent.uri, parent_cid: parent.cid
          })
          {
            "$type":     "app.bsky.feed.post",
            "text":      text_content,
            "createdAt": time.to_iso(time.now()),
            "reply": {
              "root":   { "uri": ref.root_uri,   "cid": ref.root_cid },
              "parent": { "uri": ref.parent_uri, "cid": ref.parent_cid }
            }
          }
    xrpc.post_rpc("com.atproto.repo.createRecord", {
      "repo":       did,
      "collection": "app.bsky.feed.post",
      "record":     record
    }) |> map_ok(_ -> unit)
  )

fn delete_post (rkey: str) -> Result unit str !api =
  delete_record("app.bsky.feed.post", rkey)

@private
fn delete_record (collection: str, rkey: str) -> Result unit str !api =
  with_did(did ->
    xrpc.post_rpc("com.atproto.repo.deleteRecord", {
      "repo":       did,
      "collection": collection,
      "rkey":       rkey
    }) |> map_ok(_ -> unit)
  )

@private
fn with_did (fn: str -> Result any str) -> Result any str =
  match Auth.state
    | :authenticated s -> fn(s.did)
    | _                -> :err "Not authenticated"
```

---

## api/notification.deck

```deck
fn list_notifs () -> Result [Notif] str !api =
  match xrpc.get("app.bsky.notification.listNotifications", { "limit": "30" })
    | :err e -> :err e
    | :ok body ->
        let items = get_list(body, "notifications") |> map(parse_notif)
        :ok items

fn get_unread_count () -> Result int str !api =
  match xrpc.get("app.bsky.notification.getUnreadCount", {})
    | :err e -> :err e
    | :ok body -> :ok (unwrap_opt_or(row.int(body, "count"), 0))

fn mark_seen () -> Result unit str !api =
  xrpc.post_rpc("app.bsky.notification.updateSeen", {
    "seenAt": time.to_iso(time.now())
  }) |> map_ok(_ -> unit)

@private
fn parse_notif (n: {str: any}) -> Notif =
  Notif {
    reason:     unwrap_opt_or(row.str(n, "reason"), ""),
    author:     parse_author(unwrap_opt_or(map.get(n, "author"), {})),
    is_read:    unwrap_opt_or(row.bool(n, "isRead"), false),
    indexed_at: unwrap_opt_or(row.str(n, "indexedAt"), "")
  }

@private
fn parse_author (a: {str: any}) -> Author =
  Author {
    did:          unwrap_opt_or(row.str(a, "did"),         ""),
    handle:       unwrap_opt_or(row.str(a, "handle"),      ""),
    display_name: row.str(a, "displayName"),
    avatar:       row.str(a, "avatar")
  }

@private
fn get_list (m: {str: any}, k: str) -> [any] =
  match map.get(m, k)
    | :some v when is_list(v) -> v
    | _                       -> []
```

---

## flows/login.deck

```deck
@machine LoginForm
  state :idle      (handle: str, password: str, error: str?)
  state :submitting
  initial :idle (handle: "", password: "", error: :none)

  transition :set_handle (v: str)
    from :idle s
    to   :idle (handle: v, password: s.password, error: :none)

  transition :set_password (v: str)
    from :idle s
    to   :idle (handle: s.handle, password: v, error: :none)

  transition :submit
    from :idle s
    when: not text.is_blank(s.handle) and not text.is_blank(s.password)
    to   :submitting

  transition :failed (message: str)
    from :submitting
    to   :idle (handle: "", password: "", error: :some message)


@flow LoginFlow
  state :form
    on enter ->
      match Auth.state
        | :unauthenticated (error: :some msg) -> LoginForm.send(:failed, message: msg)
        | _ -> unit
    content =
      match LoginForm.state
        | :submitting -> loading
        | :idle s ->
            match s.error
              | :some msg -> error reason: msg
              | :none     -> unit
            form
              on submit -> do_login(s.handle, s.password)
              text     :handle    value: s.handle    hint: "you.bsky.social"  format: :email
                on -> LoginForm.send(:set_handle, v: event.value)
              password :password  value: s.password  hint: "Password"
                on -> LoginForm.send(:set_password, v: event.value)
              trigger "Sign in" -> do_login(s.handle, s.password)

  initial :form

fn do_login (handle: str, password: str) -> unit !api !nvs =
  match auth.login(handle, password)
    | :ok _  -> unit
    | :err e -> LoginForm.send(:failed, message: e)
```

---

## flows/timeline.deck

```deck
@machine ThreadView
  state :loading
  state :loaded (thread: Thread)
  state :error  (message: str)
  initial :loading

  transition :loaded (thread: Thread)  from :loading  to :loaded (thread: thread)
  transition :failed (message: str)    from :loading  to :error  (message: message)


@flow TimelineFlow
  state :list
    on enter ->
      match Timeline.state
        | :empty ->
            do
              Timeline.send(:load)
              load_timeline()
        | _ -> unit
    content =
      match Timeline.state
        | :empty | :loading -> loading
        | :error s ->
            error reason: s.message
            trigger "Retry" -> do
              Timeline.send(:load)
              load_timeline()
        | :loaded s | :refreshing s | :paginating s ->
            list posts_to_show(s)
              has_more: s.cursor is :some
              on more -> load_more(s)
              p ->
                post_card(p)
            match Timeline.state
              | :paginating _ -> loading
              | _             -> unit

      create "New post" -> App.send(:open_compose)

  state :thread (uri: str)
    on enter -> load_thread(uri)
    content =
      match ThreadView.state
        | :loading -> loading
        | :error s -> error reason: s.message
        | :loaded s -> thread_node(s.thread)

  initial :list

  transition :open_thread (uri: str)
    from :list
    to   :thread (uri: uri)

  transition :close_thread
    from :thread
    to   history

fn posts_to_show (s: any) -> [Post] =
  match s
    | :loaded p    -> p.posts
    | :refreshing p -> p.posts
    | :paginating p -> p.posts
    | _             -> []

fn load_timeline () -> unit !api =
  match feed.get_timeline(:none)
    | :err e     -> Timeline.send(:failed, message: e)
    | :ok page   -> Timeline.send(:loaded, posts: page.posts, cursor: page.cursor)

fn refresh_timeline () -> unit !api =
  match feed.get_timeline(:none)
    | :err _     -> Timeline.send(:refreshed, posts: [], cursor: :none)
    | :ok page   -> Timeline.send(:refreshed, posts: page.posts, cursor: page.cursor)

fn load_more (s: any) -> unit !api =
  Timeline.send(:paginate)
  match s
    | :loaded { cursor: :some c } ->
        match feed.get_timeline(:some c)
          | :err _   -> unit
          | :ok page -> Timeline.send(:paginated, new_posts: page.posts, cursor: page.cursor)
    | _ -> unit

fn post_card (p: Post) =
  group "author"
    media unwrap_opt_or(p.author.avatar, "")
      alt:  "Avatar of {p.author.handle}"
      role: :avatar
    unwrap_opt_or(p.author.display_name, p.author.handle)
    "@{p.author.handle}"
    time_ago.format(p.created_at)
  rich_text p.text
  group "actions"
    navigate "{p.reply_count} replies" -> TimelineFlow.send(:open_thread, uri: p.uri)
    toggle :liked    state: p.liked_by_me    on -> toggle_like(p)
    toggle :reposted state: p.reposted_by_me on -> toggle_repost(p)

fn toggle_like (p: Post) -> unit !api =
  match p.liked_by_me
    | true  ->
        match p.like_rkey
          | :some rk -> interaction.unlike(rk)
          | :none    -> unit
    | false -> interaction.like(p.uri, p.cid)

fn toggle_repost (p: Post) -> unit !api =
  match p.reposted_by_me
    | true  ->
        match p.repost_rkey
          | :some rk -> interaction.unrepost(rk)
          | :none    -> unit
    | false -> interaction.repost(p.uri, p.cid)

fn thread_node (t: Thread) =
  item ->
    post_expanded(t.post)
    for reply in t.replies
      thread_node(reply)

fn post_expanded (p: Post) =
  group "author"
    media unwrap_opt_or(p.author.avatar, "")
      alt:  "Avatar of {p.author.handle}"
      role: :avatar
    unwrap_opt_or(p.author.display_name, p.author.handle)
    "@{p.author.handle}"
  rich_text p.text
  time_ago.format(p.created_at)
  group "stats"
    "{p.reply_count} replies"
    "{p.like_count} likes"
    "{p.repost_count} reposts"
  group "actions"
    toggle :liked  state: p.liked_by_me  on -> toggle_like(p)
    trigger "Reply" -> do  Compose.send(:open_reply, post: p)

fn load_thread (uri: str) -> unit !api =
  match feed.get_thread(uri)
    | :err e -> ThreadView.send(:failed, message: e)
    | :ok t  -> ThreadView.send(:loaded, thread: t)
```

---

## flows/notifications.deck

```deck
@flow NotificationsFlow
  state :list
    on enter ->
      match Notifs.state
        | :empty ->
            do
              Notifs.send(:load)
              load_notifs()
        | _ -> unit
    -- UnreadCount stream is read directly in content=; reactivity is implicit
    content =
      match Notifs.state
        | :empty | :loading -> loading
        | :error s ->
            error reason: s.message
            trigger "Retry" -> do  Notifs.send(:load)  load_notifs()
        | :loaded s ->
            match s.unread > 0
              | true ->
                  status (s.unread) label: "unread"
                  trigger "Mark all read" -> do
                    notification.mark_seen()
                    Notifs.send(:marked_read)
              | false -> unit
            list s.items
              empty ->
                "No notifications"
              n ->
                notif_row(n)

  initial :list

fn notif_row (n: Notif) =
  group "author"
    media unwrap_opt_or(n.author.avatar, "")
      alt:  "Avatar of {n.author.handle}"
      role: :avatar
    unwrap_opt_or(n.author.display_name, n.author.handle)
  reason_label(n.reason)
  time_ago.format(n.indexed_at)

fn reason_label (reason: str) -> str =
  match reason
    | "like"    -> "liked your post"
    | "repost"  -> "reposted your post"
    | "follow"  -> "followed you"
    | "mention" -> "mentioned you"
    | "reply"   -> "replied to you"
    | "quote"   -> "quoted your post"
    | r         -> r

fn load_notifs () -> unit !api =
  match notification.list_notifs()
    | :err e  -> Notifs.send(:failed, message: e)
    | :ok items ->
        let unread = count_where(items, n -> not n.is_read)
        Notifs.send(:loaded, items: items, unread: unread)
```

---

## flows/profile.deck

```deck
@machine ProfileView
  state :loading
  state :loaded  (profile: Profile, posts: [Post])
  state :error   (message: str)
  initial :loading

  transition :loaded (profile: Profile, posts: [Post])
    from :loading
    to   :loaded (profile: profile, posts: posts)
  transition :failed (message: str)
    from :loading
    to   :error (message: message)


@flow ProfileFlow
  state :profile (did: str)
    on enter -> load_profile(did)
    content =
      match ProfileView.state
        | :loading -> loading
        | :error s -> error reason: s.message
        | :loaded s ->
            item ->
              match s.profile.avatar
                | :some url ->
                    media url  alt: "Profile photo of @{s.profile.handle}"  role: :cover
                | :none -> unit
              unwrap_opt_or(s.profile.display_name, s.profile.handle)
              "@{s.profile.handle}"
              match s.profile.bio
                | :some bio -> rich_text bio
                | :none     -> unit
              group "stats"
                "{s.profile.following} following"
                "{s.profile.followers} followers"
              match is_me(s.profile.did)
                | false ->
                    match s.profile.is_following
                      | true  ->
                          confirm "Unfollow"
                            message: "Unfollow @{s.profile.handle}?"
                            -> do_unfollow(s.profile)
                      | false ->
                          trigger "Follow" -> do_follow(s.profile)
                | true -> unit
            for p in s.posts
              timeline.post_card(p)

  initial :profile (did: "")

  transition :open_profile (did: str)
    from *
    to   :profile (did: did)

fn is_me (did: str) -> bool =
  match Auth.state
    | :authenticated s -> s.did == did
    | _                -> false

fn load_profile (did: str) -> unit !api =
  let profile_result = actor.get_profile(did)
  let posts_result   = feed.get_author_feed(did, :none)
  match (profile_result, posts_result)
    | (:ok p, :ok page) -> ProfileView.send(:loaded, profile: p, posts: page.posts)
    | (:err e, _)       -> ProfileView.send(:failed, message: e)
    | (_, :err e)       -> ProfileView.send(:failed, message: e)

fn do_follow (p: Profile) -> unit !api =
  interaction.follow(p.did)

fn do_unfollow (p: Profile) -> unit !api =
  match p.follow_rkey
    | :none    -> unit
    | :some rk -> interaction.unfollow(rk)
```

---

## flows/compose.deck

```deck
-- Backed by the @machine Compose defined in app.deck.

@flow ComposeFlow
  state :editing
    content =
      match Compose.state
        | :closed -> unit

        | :open s ->
            match s.reply_to
              | :some parent ->
                  group "reply context"
                    "Replying to @{parent.author.handle}"
                    parent.text
              | :none -> unit
            text :post_text  value: s.text  max_length: 300
              on -> Compose.send(:update, text: event.value)
            "{text.length(s.text)}/300"
            trigger "Post"   -> do_post(s.text, s.reply_to)
            trigger "Cancel" -> do
              Compose.send(:cancel)
              App.send(:close_compose)

        | :posting _ -> loading

        | :error s ->
            error reason: "Failed: {s.message}"
            trigger "Retry"   -> Compose.send(:retry)
            trigger "Discard" -> do
              Compose.send(:cancel)
              App.send(:close_compose)

  initial :editing

fn do_post (text_content: str, reply_to: Post?) -> unit !api =
  Compose.send(:submit)
  match interaction.create_post(text_content, reply_to)
    | :err e -> Compose.send(:failed, message: e)
    | :ok _  -> do
        Compose.send(:done)
        App.send(:close_compose)
        timeline.refresh_timeline()
```

---

## flows/search.deck

```deck
@machine SearchState
  state :idle      (query: str)
  state :searching (query: str)
  state :results   (query: str, posts: [Post], users: [Profile])
  state :no_results (query: str)
  state :error     (query: str, message: str)

  initial :idle (query: "")

  transition :set_query (query: str)
    from *
    to   :idle (query: query)

  transition :search
    from :idle s
    when: not text.is_blank(s.query)
    to   :searching (query: s.query)

  transition :got_results (posts: [Post], users: [Profile])
    from :searching s
    when: len(posts) + len(users) == 0
    to   :no_results (query: s.query)

  transition :got_results (posts: [Post], users: [Profile])
    from :searching s
    when: len(posts) + len(users) > 0
    to   :results (query: s.query, posts: posts, users: users)

  transition :failed (message: str)
    from :searching s
    to   :error (query: s.query, message: message)


@flow SearchFlow
  state :search
    content =
      search :query  value: SearchState.state.query  hint: "Search posts and people"
        on -> SearchState.send(:set_query, query: event.value)
      trigger "Search" -> do_search(SearchState.state.query)

      match SearchState.state
        | :idle _       -> unit
        | :searching _  -> loading
        | :no_results s -> "No results for \"{s.query}\""
        | :error s      -> error reason: s.message
        | :results s    ->
            match len(s.users) > 0
              | true ->
                  group "People"
                    list s.users
                      u ->
                        group "user"
                          media unwrap_opt_or(u.avatar, "")
                            alt:  "Avatar of {u.handle}"
                            role: :avatar
                          unwrap_opt_or(u.display_name, u.handle)
                          "@{u.handle}"
                        navigate "View profile" -> do
                          MainFlow.send(:go_profile)
                          ProfileFlow.send(:open_profile, did: u.did)
              | false -> unit
            match len(s.posts) > 0
              | true ->
                  group "Posts"
                    list s.posts
                      p -> timeline.post_card(p)
              | false -> unit

  initial :search

fn do_search (q: str) -> unit !api =
  SearchState.send(:search)
  let pr = feed.search_posts(q, :none)
  let ur = actor.search_actors(q)
  SearchState.send(:got_results,
    posts: match pr | :ok page -> page.posts | :err _ -> [],
    users: match ur | :ok u -> u              | :err _ -> []
  )
```

---

## flows/settings.deck

```deck
@flow SettingsFlow
  state :main
    content =
      match Auth.state
        | :authenticated s ->
            group "Account"
              "@{s.handle}"
              s.did
            group "Feed"
              toggle :show_reposts   state: config.show_reposts   on -> unit
              toggle :notifications  state: config.notifications  on -> unit
            group "App"
              config.bsky_host
              sysinfo.app_version()
            confirm "Sign Out"  prompt: "Sign out of Bluesky?"  -> do_logout()
        | _ -> loading

  initial :main

fn do_logout () -> unit !api !nvs =
  auth.logout()
  App.send(:logged_out)
```

---

## tasks/token_refresh.deck

```deck
@task RefreshToken
  every:    4m
  when:     Auth is :authenticated
  when:     network is :connected
  priority: :normal

  run =
    do
      Auth.send(:refresh_start)
      auth.refresh()
```

---

## tasks/timeline_refresh.deck

```deck
@task RefreshTimeline
  every:    2m
  when:     Auth is :authenticated
  when:     network is :connected
  priority: :low
  battery:  :efficient

  run =
    match feed.get_timeline(:none)
      | :err _ -> unit
      | :ok page ->
          Timeline.send(:refreshed, posts: page.posts, cursor: page.cursor)
```

---

## tasks/notif_refresh.deck

```deck
@task RefreshNotifications
  every:    config.notif_interval
  when:     Auth is :authenticated
  when:     network is :connected
  priority: :low
  battery:  :efficient

  run =
    match notification.get_unread_count()
      | :err _ -> unit
      | :ok n when n > 0 ->
          match notification.list_notifs()
            | :err _ -> unit
            | :ok items ->
                do
                  let unread = count_where(items, i -> not i.is_read)
                  let prev   = match Notifs.state
                    | :loaded s -> s.unread
                    | _         -> 0
                  let suffix = match unread == 1
                    | true  -> ""
                    | false -> "s"
                  match unread > prev and config.notifications
                    | true  -> notify.send("{unread} new notification{suffix}")
                    | false -> unit
                  Notifs.send(:refreshed, items: items, unread: unread)
      | :ok _ -> unit
```

---

## Design Notes

### Memory Strategy

Posts are stored as `[Post]` (typed `@type` records) in machine state rather than raw `{str: any}` maps. This is intentional:
- Field access is O(1) and type-safe
- The interpreter can share identical `Author` record instances across posts by the same author
- No helper functions needed to extract fields — `p.text`, `p.author.handle` directly

The `cache` capability holds API response bodies (strings/JSON) for 30 seconds. The `Timeline` machine holds parsed `Post` records in memory. The cache and the machine state are complementary: cache reduces network calls, machine state provides fast UI access.

### Auth Flow

```
LoginFlow :form — user fills handle + password → button press
  auth.login()
  ├── xrpc.post_rpc("createSession")
  ├── parse_session() → Session @type
  ├── nvs.set("bsky_session", json)   -- survive restart
  ├── api.set_token(access_jwt)       -- configure api_client
  └── Auth.send(:success)             -- machine → :authenticated
                                      -- @flow App :authenticated transition fires
                                      -- App moves from :login to :main (MainFlow)

RefreshToken task (every 4m):
  Auth.send(:refresh_start)
  auth.refresh()
  ├── api.set_token(refresh_jwt)
  ├── xrpc.post_rpc("refreshSession")
  ├── parse and save new session
  ├── api.set_token(new_access_jwt)
  └── Auth.send(:refresh_ok)

Token expiry (api returns :unauthorized):
  xrpc helpers return :err "Session expired"
  Tasks fail silently → retry next cycle
  When user triggers manual action → error shown in UI
  User manually signs out and back in
```

### ATProto Notes

- All XRPC calls use a single `api_client` instance configured once in `@on launch`
- The app always talks to `config.bsky_host` — no PDS discovery (sufficient for Bluesky network)
- `rkey` extraction: last segment of `at://` URI, used for delete operations
- Reply `root` reference: taken from the parent post's existing `reply_ref` if it has one (thread root), otherwise the parent itself is the root
- Parsing is done eagerly at API call time into `@type` records, keeping flow content= bodies clean
