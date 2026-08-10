# Photon Browser

A lightweight browser. On Linux it's built on GTK3 + WebKitGTK; on Windows,
Win32 + Microsoft Edge WebView2. Both share the same design: the toolbar,
tabs, and address bar are an HTML/CSS/JS page talking to native code, and
each tab's actual web content is a separate native WebView.

No native window titlebar — tabs and the toolbar use the full top of the
window, with window controls and dragging built into the HTML chrome itself.

## Features

- Tabs with favicons, drag-to-reorder, adjustable size
- Address bar doubles as a search box, with live suggestions as you type
- Bookmarks
- Built-in adblocker (WebKit's native content-blocker engine)
- Extensions: a userscript/userstyle manager (add JS/CSS snippets scoped to
  a URL pattern) — not real browser extensions, since neither engine exposes
  a public API for loading actual Chrome/Firefox-format extensions
- Cookie editor
- Themes: Dark (default), Light, or Custom color
- Settings for search engine, language (English/Русский), toolbar layout,
  and RAM usage

## Linux vs Windows

- **Linux**: GTK3 + WebKitGTK. Single binary, no installer.
- **Windows**: Win32 + WebView2 (Chromium, comes with Windows 10/11).
  Separate codebase from the Linux version — WebKitGTK isn't available on
  Windows, so there's no way to share the rendering engine between them.

## Build (Linux)

```
sudo pacman -S --needed gtk3 webkit2gtk-4.1 base-devel pkgconf
make
./photon-browser
```

## Build (Windows)

See `windows/README.md`.
