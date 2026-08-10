# Photon Browser for Windows

Win32 + Microsoft Edge WebView2. A separate program from the Linux
version — WebKitGTK has no Windows port, so there's no way to share the
rendering engine between them. Uses [webview/webview](https://github.com/webview/webview)
(MIT, vendored as `webview.h` in this directory via its own amalgamation
script) for the WebView2 COM setup.

## Status: core only, not full parity with Linux

Implemented: borderless window with custom drag/minimize/maximize/close,
tabs, address bar (Enter to navigate or search), back/forward/reload.

Not yet implemented: settings persistence, bookmarks, cookie editor,
extensions, adblock, search suggestions, tab favicons/live titles. This
was written, and never compiled, from this repository's Linux-only dev
environment — GitHub Actions (`.github/workflows/release.yml`) builds it
for real on a Windows runner, which is the first actual compile it gets.
Expect to file/fix issues once it's actually run.

## Build

Requires Visual Studio 2022 (Desktop development with C++ workload) and
the [Microsoft.Web.WebView2](https://www.nuget.org/packages/Microsoft.Web.WebView2)
NuGet package (WebView2 itself ships with Windows 10/11; the SDK package
just provides headers/import libs at build time). See
`.github/workflows/release.yml` in the repo root for the exact CI build
steps (NuGet restore + `cl.exe` invocation) if setting this up manually.
