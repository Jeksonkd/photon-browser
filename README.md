<p align="center"><img src="assets/logo.png" alt="Photon Light" width="120"></p>

# Photon Light

A lightweight Linux browser built on GTK3 + WebKitGTK. The toolbar, tabs,
and address bar are an HTML/CSS/JS page talking to native code, and each
tab's actual web content is a separate native WebView.

No native window titlebar — tabs and the toolbar use the full top of the
window, with window controls and dragging built into the HTML chrome itself.

This is the WebKitGTK sibling of [Photon Browser](https://github.com/Jeksonkd/photon-browser),
which moved to a Chromium-based engine. Photon Light stays on WebKitGTK: no
bundled browser engine, a binary a few hundred KB instead of hundreds of MB.

## Features

- Tabs with favicons, drag-to-reorder, adjustable size
- Address bar doubles as a search box, with live suggestions as you type
- Bookmarks, plus an optional bookmarks bar
- Built-in adblocker (WebKit's native content-blocker engine)
- Extensions: a userscript/userstyle manager (add JS/CSS snippets scoped to
  a URL pattern, optionally with a toolbar button + menu of buttons/
  checkboxes/sliders) — not real browser extensions, since WebKitGTK exposes
  no public API for loading actual Chrome/Firefox-format extensions
- Cookie editor
- Themes: Dark (default), Light, or Custom color
- Settings for search engine, language (English/Русский), toolbar layout,
  and RAM usage

## Build

```
sudo pacman -S --needed gtk3 webkit2gtk-4.1 base-devel pkgconf
make
./photon-light
```
