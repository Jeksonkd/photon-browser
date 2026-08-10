# Photon Browser

A minimal browser built on GTK3 + WebKitGTK. The chrome (tab strip,
toolbar, address bar) is an HTML/CSS/JS page rendered in its own WebView —
tabs sit at the top, the address/toolbar row directly below them — talking
to native C++ via `postMessage`/`evaluateJavascript`. Real page content is
a native WebView per tab, switched via a `GtkStack`. Settings, the Cookie
Editor, Plugins, and Bookmarks are native GTK panels shown the same way.

The window has no native titlebar at all — window controls (minimize/
maximize/close) and drag-to-move live in the HTML chrome itself, so tabs
and the toolbar get the full top of the window. One consequence: since
there's no native border either, resizing relies on whatever your window
manager provides for undecorated windows (usually a corner/edge drag
region or a keyboard/menu shortcut) rather than a click-and-drag titlebar
border.

Written in C++ directly against the GTK3/WebKit2GTK C APIs (no Python, no
gtkmm, no JSON library — the native↔JS protocol is a handful of plain
strings). The compiled binary is a native executable under 300KB.

**Linux only for now.** WebKitGTK has no maintained Windows build. A
separate native Windows front end (Win32 + Edge WebView2) is a distinct,
not-yet-started project — see "Windows" below.

## Features

- Tabs (rendered in the HTML chrome): new/close/drag-to-reorder, favicons (fetched async
  and pushed into the tab strip as base64 PNGs), `window.open()`/
  `target=_blank` opens a tab instead of a popup window. Tabs flex to fill
  available width and shrink as more open (down to a minimum, then the
  strip scrolls). Size and corner rounding are both adjustable (Settings →
  Toolbar → Tab size; Settings → Appearance → Tab roundness).
  - **New tabs**: the pill is added immediately, but the actual
    `WebKitWebView` construction/realize (the genuinely slow part — GTK
    realizing a WebView inside an already-visible window is where WebKit's
    own per-view setup cost lands) is deferred to the next GTK idle cycle
    via `finish_new_tab()`/`new_tab()`, so the pill gets an actual frame to
    render before that work blocks anything. `window.open()`/popups can't
    use this path — WebKit's `create` signal needs a real, usable view
    returned synchronously — so only plain new tabs defer. There's a brief
    window where a pending tab's `view`/`page` are still null; every reader
    of those fields already guards for it (including `close_tab()`, in case
    a tab is closed before its own creation finishes).
- Address bar doubles as a search box, with live suggestions as you type
  (arrow keys to navigate, Enter/click to pick) fetched from the configured
  engine's own suggestion API — DuckDuckGo or Google, engine configurable
  in Settings → Search. Fetched natively via libsoup rather than from the
  chrome's own JS, since the chrome page has no real origin and a
  cross-origin `fetch()` from there would be at the mercy of the target's
  CORS headers.
- **Built-in adblocker** (Settings → Privacy → Block ads & trackers, on by
  default): uses WebKit's native content-blocker engine — the same
  mechanism Safari's content blockers use — with a curated list of common
  ad/tracker domains. Not EasyList-comprehensive, but genuinely blocks at
  the network-request level rather than hiding elements with CSS.
- **Bookmarks**: the "···" button bookmarks/unbookmarks the current page
  via a small HTML dropdown, or opens the Bookmarks tab to browse, open,
  or delete saved pages (favicons fetched from WebKit's favicon cache).
- **Appearance** (Settings → Appearance): theme is Dark (default — a
  custom, deliberately darker palette than stock Adwaita-dark), Light
  (true white with dark text), or Custom (pick your own chrome background
  *and* text color; both pickers only appear when Custom is selected).
  Every native widget's colors (labels, buttons, entries, checkboxes) are
  set explicitly per theme rather than inherited from your system GTK
  theme — otherwise Light mode would render with whatever text color your
  desktop's *current* theme happens to use, dark or not.
- **Settings tab** (gear icon, laid out as cards): Privacy (clear all site
  data on close or on demand), Cookies (opens the Cookie Editor), Plugins
  (opens the Plugin manager), Search engine, Appearance, Language
  (English/Русский, applied live), and Toolbar (button size + position —
  up/down reorder of Back/Forward/Reload/Address bar/Settings/Bookmarks).
- **Cookie Editor** (opened from Settings → Cookies): lists every stored
  cookie (domain, name, value). Add, edit, or delete individual cookies,
  or wipe all of them. Editing deletes the original and re-adds it with a
  1-year lifetime — expiry/secure/httpOnly flags aren't exposed in the UI.
- **Plugins** (opened from Settings → Plugins): a userscript/userstyle
  manager — add named JS or CSS snippets, each optionally scoped to a
  WebKit match pattern (e.g. `https://example.com/*`; empty = all sites),
  toggle them on/off, edit or delete. Applied to every open tab immediately
  and to all new tabs. WebKitGTK has no public API for loading real
  Chrome/Firefox-format extensions from an extension store — this is the
  closest workable alternative, built on the same script-injection API the
  chrome bridge itself uses.
- Persistent cookies/cache/favicons across restarts, stored in
  `~/.local/share/photon-browser/`. Settings, bookmarks, and plugins are
  stored in `~/.config/photon-browser/config.ini`.
- Memory footprint: WebKit is a full modern rendering engine (JIT JS, layout,
  GPU compositor) — tens of MB of baseline overhead per tab is structural,
  not something a settings flag removes. What's actually tunable:
  `WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER` instead of the default `WEB_BROWSER`
  model (caches a moderate number of resources instead of "a very large
  number... and previously viewed content"); the back-forward page cache
  disabled (back/forward does a real reload instead of an instant restore
  from a fully-rendered page held in memory); and hardware acceleration off
  (`NEVER`) by default. Neither forcing it `ALWAYS` on nor WebKit's own
  adaptive `ON_DEMAND` policy turned out to make new-tab creation faster —
  see "New tabs" below for what actually did. **RAM saving mode** (Settings
  → Privacy) still exists and still forces `NEVER` explicitly, though it's
  currently equivalent to the default either way. Forcing all tabs into a
  single shared WebProcess
  instead of one process per tab isn't available: that
  API — `webkit_web_context_set_process_model()` — has been deprecated
  since WebKit 2.40, upstream having made process-per-view mandatory for
  security, so it's a no-op on current WebKitGTK.

## Not carried over from the native-toolbar version

Moving the chrome to HTML was a large rewrite. Button size and position
(reorder) are back (Settings → Toolbar). Not restored:

- **Per-button show/hide and per-button recolor** — every button in the
  order list is always visible, and only whole-chrome theming (not
  individual button colors) is supported. Could come back if wanted.
- **Compact mode** — superseded by the new default layout (tabs above the
  toolbar), which was the reason compact mode existed in the first place.

## Build

```
sudo pacman -S --needed gtk3 webkit2gtk-4.1 base-devel pkgconf
make
```

## Run

```
./photon-browser
# or open a specific site on launch:
./photon-browser example.com
```

## Windows

Not implemented yet. WebKitGTK has no supported Windows build, so the
Windows version will be a separate program: native Win32 UI + Microsoft
Edge WebView2 (Chromium, preinstalled on Windows 10/11) for the web
engine, sharing the settings file format but none of the GTK code. It
needs to be built and tested on an actual Windows machine — there's no
way to compile or run it from this Linux session.
