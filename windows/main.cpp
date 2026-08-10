// Photon Browser for Windows: Win32 + Microsoft Edge WebView2.
//
// Separate program from the Linux (GTK3 + WebKitGTK) version -- WebKitGTK
// has no Windows port, so there's no way to share the rendering engine.
// The architecture mirrors the Linux version conceptually: an HTML/CSS/JS
// chrome (tab strip + toolbar) hosted in its own WebView, talking to
// native code via a bound JS function; each tab's actual page content is
// a separate native WebView, shown/hidden by resizing/toggling its host
// child window.
//
// Uses the vendored webview.h (MIT, github.com/webview/webview) for the
// WebView2 COM bootstrapping, rather than hand-written COM interop that
// can't be verified without a Windows compiler in hand.
//
// THIS IS A CORE, NOT FEATURE PARITY WITH LINUX. Not yet implemented:
// settings persistence, bookmarks, cookie editor, extensions, adblock,
// search suggestions. See windows/README.md.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "webview.h"

static const wchar_t *MAIN_CLASS = L"PhotonBrowserMain";
static const wchar_t *CHILD_CLASS = L"PhotonBrowserChild";
static const int CHROME_HEIGHT = 86;
static const wchar_t *HOME_URL = L"https://duckduckgo.com/html/";

struct Tab {
  int id = 0;
  HWND hwnd = nullptr;
  webview_t view = nullptr;
  std::wstring title = L"New Tab";
};

struct App {
  HWND main_hwnd = nullptr;
  HWND chrome_hwnd = nullptr;
  webview_t chrome = nullptr;
  std::vector<std::unique_ptr<Tab>> tabs;
  Tab *current = nullptr;
  int next_id = 1;
};

static App g_app;

// -- helpers ----------------------------------------------------------

static std::string narrow(const std::wstring &w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

static std::wstring widen(const std::string &s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
  return w;
}

// Same trick used by the library's own JS glue: a JS string literal is
// safest built by escaping quotes/backslashes/control chars.
static std::string js_string_literal(const std::string &s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += "\"";
  return out;
}

static void run_chrome_js(const std::string &code) {
  if (g_app.chrome) webview_eval(g_app.chrome, code.c_str());
}

static Tab *find_tab_by_id(int id) {
  for (auto &t : g_app.tabs) {
    if (t->id == id) return t.get();
  }
  return nullptr;
}

// -- layout -------------------------------------------------------------

static void layout() {
  RECT rc;
  GetClientRect(g_app.main_hwnd, &rc);
  int width = rc.right - rc.left;
  int height = rc.bottom - rc.top;

  if (g_app.chrome_hwnd) MoveWindow(g_app.chrome_hwnd, 0, 0, width, CHROME_HEIGHT, TRUE);
  int content_h = height - CHROME_HEIGHT;
  if (content_h < 0) content_h = 0;
  for (auto &t : g_app.tabs) {
    if (t->hwnd) MoveWindow(t->hwnd, 0, CHROME_HEIGHT, width, content_h, TRUE);
  }
}

// -- tabs ---------------------------------------------------------------

static void switch_to_tab(Tab *tab) {
  if (!tab) return;
  g_app.current = tab;
  for (auto &t : g_app.tabs) ShowWindow(t->hwnd, t.get() == tab ? SW_SHOW : SW_HIDE);
  SetWindowTextW(g_app.main_hwnd, (tab->title + L" — Photon Browser").c_str());
  run_chrome_js("photonSetActiveTab(" + std::to_string(tab->id) + ")");
}

static void close_tab(Tab *tab) {
  bool was_current = (g_app.current == tab);
  if (was_current) g_app.current = nullptr;

  run_chrome_js("photonRemoveTab(" + std::to_string(tab->id) + ")");

  if (tab->view) webview_destroy(tab->view);
  if (tab->hwnd) DestroyWindow(tab->hwnd);

  for (auto it = g_app.tabs.begin(); it != g_app.tabs.end(); ++it) {
    if (it->get() == tab) {
      g_app.tabs.erase(it);
      break;
    }
  }

  if (g_app.tabs.empty()) {
    DestroyWindow(g_app.main_hwnd);
    return;
  }
  if (was_current) switch_to_tab(g_app.tabs.back().get());
}

static Tab *new_tab(const std::wstring &url, bool switch_to) {
  auto tab = std::make_unique<Tab>();
  tab->id = g_app.next_id++;

  RECT rc;
  GetClientRect(g_app.main_hwnd, &rc);
  tab->hwnd = CreateWindowExW(0, CHILD_CLASS, L"", WS_CHILD,
                               0, CHROME_HEIGHT, rc.right - rc.left, rc.bottom - rc.top - CHROME_HEIGHT,
                               g_app.main_hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
  tab->view = webview_create(0, tab->hwnd);

  Tab *raw = tab.get();
  run_chrome_js("photonAddTab(" + std::to_string(raw->id) + "," + js_string_literal(narrow(raw->title)) + ")");

  if (raw->view) {
    webview_navigate(raw->view, narrow(url).c_str());
  }

  g_app.tabs.push_back(std::move(tab));
  if (switch_to) switch_to_tab(raw);
  return raw;
}

// -- native <- chrome JS bridge -------------------------------------------

static void on_chrome_message(const char *id, const char *req, void *arg) {
  // req is a JSON array with our single string argument, e.g. ["back"].
  std::string msg = webview::detail::json_parse(req, "", 0);
  auto starts_with = [&](const char *prefix) { return msg.rfind(prefix, 0) == 0; };

  Tab *tab = g_app.current;
  if (msg == "back") {
    if (tab && tab->view) webview_eval(tab->view, "history.back()");
  } else if (msg == "forward") {
    if (tab && tab->view) webview_eval(tab->view, "history.forward()");
  } else if (msg == "reload") {
    if (tab && tab->view) webview_eval(tab->view, "location.reload()");
  } else if (msg == "newTab") {
    new_tab(HOME_URL, true);
  } else if (starts_with("switchTab:")) {
    if (Tab *t = find_tab_by_id(atoi(msg.c_str() + 10))) switch_to_tab(t);
  } else if (starts_with("closeTab:")) {
    if (Tab *t = find_tab_by_id(atoi(msg.c_str() + 9))) close_tab(t);
  } else if (starts_with("navigate:")) {
    std::string text = msg.substr(9);
    if (tab && tab->view) {
      // Bare heuristic: treat it as a URL if it looks like one, else search.
      std::string target;
      if (text.find("://") != std::string::npos) {
        target = text;
      } else if (text.find('.') != std::string::npos && text.find(' ') == std::string::npos) {
        target = "https://" + text;
      } else {
        target = "https://duckduckgo.com/html/?q=" + text;  // caller already encodeURIComponent'd it
      }
      webview_navigate(tab->view, target.c_str());
    }
  } else if (msg == "winMinimize") {
    ShowWindow(g_app.main_hwnd, SW_MINIMIZE);
  } else if (msg == "winMaximize") {
    WINDOWPLACEMENT wp = {sizeof(wp)};
    GetWindowPlacement(g_app.main_hwnd, &wp);
    ShowWindow(g_app.main_hwnd, wp.showCmd == SW_SHOWMAXIMIZED ? SW_RESTORE : SW_SHOWMAXIMIZED);
  } else if (msg == "winClose") {
    DestroyWindow(g_app.main_hwnd);
  } else if (msg == "winDrag") {
    ReleaseCapture();
    SendMessageW(g_app.main_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
  }

  // Resolves the Promise the JS-side bound call returns; our own JS never
  // awaits it, but leaving it permanently pending would accumulate pending
  // call records in the library over a long session.
  webview_return(static_cast<webview_t>(arg), id, 0, "");
}

// -- chrome HTML (Windows v1: trimmed to what's actually implemented here) -

static const char *CHROME_HTML = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
:root { --bg:#18191c; --bg2:#1a1b1e; --fg:#e8e8e8; --fg-dim:#a9a9ad; --tab-bg:#202124;
        --tab-active:#2b2c30; --accent:#3a7afe; --border:rgba(128,128,128,0.25); }
* { box-sizing:border-box; }
html,body { margin:0; padding:0; height:100%; overflow:hidden; background:var(--bg2); color:var(--fg);
            font-family:Segoe UI,-apple-system,sans-serif; font-size:13px; -webkit-user-select:none; user-select:none; }
#tabs { display:flex; align-items:stretch; gap:4px; height:44px; padding:5px 5px 0; background:var(--bg); overflow-x:auto; }
.tab { display:flex; align-items:center; gap:8px; padding:0 12px; flex:1 1 0; min-width:96px; max-width:260px;
       background:var(--tab-bg); color:var(--fg-dim); border-radius:6px 6px 0 0; font-size:14px; }
.tab.active { background:var(--tab-active); color:var(--fg); }
.title { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; flex:1; }
.close { opacity:0.55; padding:1px 5px; border-radius:3px; }
.close:hover { opacity:1; background:rgba(128,128,128,0.35); }
#newtab { display:flex; align-items:center; justify-content:center; width:38px; color:var(--fg-dim); font-size:21px; font-weight:600; }
#newtab:hover { color:var(--fg); background:rgba(128,128,128,0.15); }
#toolbar { display:flex; align-items:center; gap:4px; height:42px; padding:0 6px; background:var(--bg2); border-top:1px solid var(--border); }
#toolbar button { border:none; background:none; color:var(--fg); width:28px; height:28px; border-radius:6px;
                   display:flex; align-items:center; justify-content:center; font-size:15px; }
#toolbar button:hover { background:rgba(128,128,128,0.2); }
#address { flex:1; height:28px; border-radius:6px; border:1px solid var(--border); background:var(--tab-bg);
           color:var(--fg); padding:0 10px; font-size:13px; outline:none; user-select:text; }
#dragregion { flex:1; align-self:stretch; }
#wincontrols { display:flex; align-items:stretch; }
.winbtn { display:flex; align-items:center; justify-content:center; width:38px; color:var(--fg-dim); font-size:13px; }
.winbtn:hover { background:rgba(128,128,128,0.2); color:var(--fg); }
.winbtn-close:hover { background:#e81123; color:#fff; }
</style></head>
<body>
<div id="tabs"><div id="newtab" title="New Tab">+</div></div>
<div id="toolbar">
  <button id="back" title="Back">&#8249;</button>
  <button id="forward" title="Forward">&#8250;</button>
  <button id="reload" title="Reload">&#8635;</button>
  <input id="address" type="text" spellcheck="false">
  <div id="dragregion"></div>
  <div id="wincontrols">
    <div class="winbtn" id="win-min" title="Minimize">&#8212;</div>
    <div class="winbtn" id="win-max" title="Maximize">&#9633;</div>
    <div class="winbtn winbtn-close" id="win-close" title="Close">&#10005;</div>
  </div>
</div>
<script>
function send(msg) { window.nativeSend(msg); }
document.getElementById('back').addEventListener('click', function(){ send('back'); });
document.getElementById('forward').addEventListener('click', function(){ send('forward'); });
document.getElementById('reload').addEventListener('click', function(){ send('reload'); });
document.getElementById('newtab').addEventListener('click', function(){ send('newTab'); });
document.getElementById('win-min').addEventListener('click', function(){ send('winMinimize'); });
document.getElementById('win-max').addEventListener('click', function(){ send('winMaximize'); });
document.getElementById('win-close').addEventListener('click', function(){ send('winClose'); });
document.getElementById('dragregion').addEventListener('mousedown', function(e){ if (e.button===0) send('winDrag'); });
document.getElementById('dragregion').addEventListener('dblclick', function(){ send('winMaximize'); });

var addressEl = document.getElementById('address');
addressEl.addEventListener('keydown', function(e) {
  if (e.key === 'Enter') send('navigate:' + encodeURIComponent(addressEl.value));
});

function photonAddTab(id, title) {
  var el = document.createElement('div');
  el.className = 'tab';
  el.dataset.id = id;
  var titleEl = document.createElement('span');
  titleEl.className = 'title';
  titleEl.textContent = title;
  var closeEl = document.createElement('span');
  closeEl.className = 'close';
  closeEl.textContent = '×';
  closeEl.addEventListener('click', function(e){ e.stopPropagation(); send('closeTab:' + id); });
  el.appendChild(titleEl); el.appendChild(closeEl);
  el.addEventListener('click', function(){ send('switchTab:' + id); });
  document.getElementById('tabs').insertBefore(el, document.getElementById('newtab'));
}
function photonUpdateTab(id, title) {
  var el = document.querySelector('.tab[data-id="' + id + '"]');
  if (el) el.querySelector('.title').textContent = title;
}
function photonRemoveTab(id) {
  var el = document.querySelector('.tab[data-id="' + id + '"]');
  if (el) el.remove();
}
function photonSetActiveTab(id) {
  document.querySelectorAll('.tab').forEach(function(el) { el.classList.toggle('active', el.dataset.id == id); });
}
function photonSetAddress(url) {
  if (document.activeElement !== addressEl) addressEl.value = url;
}
</script>
</body></html>
)HTML";

// -- window procs ---------------------------------------------------------

static LRESULT CALLBACK ChildProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SIZE:
      layout();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// -- entry point ------------------------------------------------------

// Resource ID 101 matches app.rc -- keep them in sync.
#define IDI_APPICON 101

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
  HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));

  WNDCLASSEXW mainClass = {sizeof(mainClass)};
  mainClass.lpfnWndProc = MainProc;
  mainClass.hInstance = hInstance;
  mainClass.lpszClassName = MAIN_CLASS;
  mainClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  mainClass.hbrBackground = CreateSolidBrush(RGB(0x1a, 0x1b, 0x1e));
  mainClass.hIcon = appIcon;
  mainClass.hIconSm = appIcon;
  RegisterClassExW(&mainClass);

  WNDCLASSEXW childClass = {sizeof(childClass)};
  childClass.lpfnWndProc = ChildProc;
  childClass.hInstance = hInstance;
  childClass.lpszClassName = CHILD_CLASS;
  childClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExW(&childClass);

  // WS_POPUP: no native titlebar/border, matching the Linux version --
  // window controls and dragging live in the HTML chrome instead.
  g_app.main_hwnd = CreateWindowExW(0, MAIN_CLASS, L"Photon Browser", WS_POPUP | WS_VISIBLE,
                                     CW_USEDEFAULT, CW_USEDEFAULT, 1100, 750,
                                     nullptr, nullptr, hInstance, nullptr);

  g_app.chrome_hwnd = CreateWindowExW(0, CHILD_CLASS, L"", WS_CHILD | WS_VISIBLE,
                                       0, 0, 1100, CHROME_HEIGHT, g_app.main_hwnd, nullptr, hInstance, nullptr);
  g_app.chrome = webview_create(0, g_app.chrome_hwnd);
  webview_bind(g_app.chrome, "nativeSend", on_chrome_message, g_app.chrome);
  webview_set_html(g_app.chrome, CHROME_HTML);

  new_tab(HOME_URL, true);

  layout();
  ShowWindow(g_app.main_hwnd, nCmdShow);
  UpdateWindow(g_app.main_hwnd);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  for (auto &t : g_app.tabs) {
    if (t->view) webview_destroy(t->view);
  }
  if (g_app.chrome) webview_destroy(g_app.chrome);
  return 0;
}
