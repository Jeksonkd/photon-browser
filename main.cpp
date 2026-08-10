// Photon Browser: a tiny native GTK3 + WebKitGTK browser.
//
// The chrome (tab strip + toolbar + address bar) is an HTML/CSS/JS page
// rendered in its own WebView, talking to native code via
// window.webkit.messageHandlers.photon.postMessage() (JS -> native) and
// webkit_web_view_evaluate_javascript() (native -> JS). This is what lets
// the chrome use plain flexbox to put tabs above the address bar -- GTK's
// own notebook widget can't put anything between its tab row and its pages.
//
// Settings, the Cookie Editor and Bookmarks stay native GTK panels shown in
// a GtkStack alongside the real per-tab WebViews; none of that needed to
// change for the chrome rewrite.

#include <cairo.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <regex>
#include <string>
#include <vector>

// App/window icon, embedded so it doesn't depend on a file next to the
// binary -- this is a single portable executable (AppImage, no installer).
#include "icon_data.h"

// -- i18n -------------------------------------------------------------

enum class Str {
    NewTab, Settings, Privacy, ClearOnExit, ClearNow, ClearDone, Language,
    Cookies, Reset, Search, SearchEngine, AddCookie, EditCookie, DeleteAllCookies,
    RefreshCookies, CookiesWord, NameField, ValueField, Domain, PathField,
    Save, Cancel, Bookmarks, BookmarkThis, RemoveBookmark, ManageBookmarks,
    Appearance, Theme, ThemeLight, ThemeDark, ThemeCustom, CustomColor,
    OpenCookieEditor, Toolbar, Back, Forward, Reload, AddressBar, Size,
    TextColor, TabShape, Extensions, AddExtension, EditExtension, MatchPattern, ExtensionType,
    Enabled, ManageExtensions, BlockAds, TabSize, RamSavingMode
};

static const char *tr(Str key, const std::string &lang) {
    static const std::map<Str, std::array<const char *, 2>> table = {
        {Str::NewTab, {"New Tab", "Новая вкладка"}},
        {Str::Settings, {"Settings", "Настройки"}},
        {Str::Privacy, {"Privacy", "Приватность"}},
        {Str::ClearOnExit,
         {"Clear cookies & site data when the browser closes",
          "Очищать куки и данные сайтов при закрытии браузера"}},
        {Str::ClearNow, {"Clear data now", "Очистить данные сейчас"}},
        {Str::ClearDone, {"Data cleared.", "Данные очищены."}},
        {Str::Language, {"Language", "Язык"}},
        {Str::Cookies, {"Cookies", "Куки"}},
        {Str::Reset, {"Reset", "Сбросить"}},
        {Str::Search, {"Search", "Поиск"}},
        {Str::SearchEngine, {"Default search engine", "Поисковая система по умолчанию"}},
        {Str::AddCookie, {"Add cookie", "Добавить куки"}},
        {Str::EditCookie, {"Edit cookie", "Изменить куки"}},
        {Str::DeleteAllCookies, {"Delete all cookies", "Удалить все куки"}},
        {Str::RefreshCookies, {"Refresh", "Обновить"}},
        {Str::CookiesWord, {"cookies", "куки"}},
        {Str::NameField, {"Name", "Имя"}},
        {Str::ValueField, {"Value", "Значение"}},
        {Str::Domain, {"Domain", "Домен"}},
        {Str::PathField, {"Path", "Путь"}},
        {Str::Save, {"Save", "Сохранить"}},
        {Str::Cancel, {"Cancel", "Отмена"}},
        {Str::Bookmarks, {"Bookmarks", "Закладки"}},
        {Str::BookmarkThis, {"Bookmark this page", "Добавить в закладки"}},
        {Str::RemoveBookmark, {"Remove bookmark", "Удалить из закладок"}},
        {Str::ManageBookmarks, {"Manage bookmarks", "Управление закладками"}},
        {Str::Appearance, {"Appearance", "Внешний вид"}},
        {Str::Theme, {"Theme", "Тема"}},
        {Str::ThemeLight, {"Light", "Светлая"}},
        {Str::ThemeDark, {"Dark", "Тёмная"}},
        {Str::ThemeCustom, {"Custom", "Своя"}},
        {Str::CustomColor, {"Custom color", "Свой цвет"}},
        {Str::OpenCookieEditor, {"Manage cookies", "Управление куками"}},
        {Str::Toolbar, {"Toolbar", "Панель инструментов"}},
        {Str::Back, {"Back", "Назад"}},
        {Str::Forward, {"Forward", "Вперёд"}},
        {Str::Reload, {"Reload", "Обновить"}},
        {Str::AddressBar, {"Address Bar", "Адресная строка"}},
        {Str::Size, {"Button size", "Размер кнопок"}},
        {Str::TextColor, {"Text color", "Цвет текста"}},
        {Str::TabShape, {"Tab roundness", "Скругление вкладок"}},
        {Str::Extensions, {"Extensions", "Расширения"}},
        {Str::AddExtension, {"Add extension", "Добавить расширение"}},
        {Str::EditExtension, {"Edit extension", "Изменить расширение"}},
        {Str::MatchPattern, {"Match pattern (empty = all sites)", "Шаблон адреса (пусто = все сайты)"}},
        {Str::ExtensionType, {"Type", "Тип"}},
        {Str::Enabled, {"Enabled", "Включён"}},
        {Str::ManageExtensions, {"Manage extensions", "Управление расширениями"}},
        {Str::BlockAds, {"Block ads & trackers", "Блокировать рекламу и трекеры"}},
        {Str::RamSavingMode,
         {"RAM saving mode (locks hardware acceleration off)", "Режим экономии ОЗУ (аппаратное ускорение всегда выключено)"}},
        {Str::TabSize, {"Tab size", "Размер вкладок"}},
    };
    int idx = (lang == "ru") ? 1 : 0;
    return table.at(key)[idx];
}

// -- search engines -------------------------------------------------------

struct SearchEngine {
    const char *id;
    const char *name;
    const char *home;
    const char *search_prefix;
    const char *suggest_prefix;  // append URL-encoded query; response format differs per engine, see parse_suggestions()
};

static const std::vector<SearchEngine> SEARCH_ENGINES = {
    {"duckduckgo", "DuckDuckGo", "https://duckduckgo.com/html/", "https://duckduckgo.com/html/?q=",
     "https://duckduckgo.com/ac/?type=list&q="},
    {"google", "Google", "https://www.google.com/", "https://www.google.com/search?q=",
     "https://www.google.com/complete/search?client=firefox&q="},
};

// -- bookmarks --------------------------------------------------------

struct Bookmark {
    std::string url;
    std::string title;
};

static const std::vector<std::string> TOOLBAR_IDS = {"back", "forward", "reload", "address", "settings", "bookmarks"};

// -- extensions (userscripts / userstyles) --------------------------------

struct Extension {
    std::string name;
    std::string match;  // WebKit match pattern, e.g. "https://example.com/*"; empty = all sites
    std::string type;   // "js" | "css"
    std::string code;
    bool enabled = true;
};

// -- settings persistence ----------------------------------------------

struct Settings {
    bool clear_on_exit = false;
    std::string language = "en";
    std::string search_engine = "duckduckgo";
    std::string theme = "dark";  // "dark" | "light" | "custom"
    std::string bg_color;        // used only when theme == "custom"
    std::string fg_color;        // used only when theme == "custom"
    int tab_radius = 6;          // px, tab corner rounding
    int tab_height = 44;         // px, tab strip row height
    int toolbar_size = 28;       // px, applied to toolbar buttons and the address bar
    bool adblock_enabled = true;
    bool ram_saving_mode = false;  // locks hardware acceleration off for every page, not just on demand
    std::vector<std::string> toolbar_order = {"back", "forward", "reload", "address", "settings", "bookmarks"};
    std::vector<Bookmark> bookmarks;
    std::vector<Extension> extensions;
};

static std::string config_dir() { return std::string(g_get_user_config_dir()) + "/photon-browser"; }
static std::string config_path() { return config_dir() + "/config.ini"; }

static std::string detect_system_language() {
    const gchar *const *langs = g_get_language_names();
    if (langs && langs[0] && std::string(langs[0]).rfind("ru", 0) == 0) return "ru";
    return "en";
}

static Settings load_settings() {
    Settings s;
    s.language = detect_system_language();
    GKeyFile *kf = g_key_file_new();
    bool loaded = g_key_file_load_from_file(kf, config_path().c_str(), G_KEY_FILE_NONE, nullptr);
    if (loaded) {
        GError *err = nullptr;
        gboolean v = g_key_file_get_boolean(kf, "general", "clear_on_exit", &err);
        if (!err) s.clear_on_exit = v;
        g_clear_error(&err);

        gchar *lang = g_key_file_get_string(kf, "general", "language", &err);
        if (!err && lang) s.language = lang;
        g_clear_error(&err);
        g_free(lang);

        gchar *engine = g_key_file_get_string(kf, "general", "search_engine", &err);
        if (!err && engine) s.search_engine = engine;
        g_clear_error(&err);
        g_free(engine);

        gchar *theme = g_key_file_get_string(kf, "general", "theme", &err);
        if (!err && theme) s.theme = theme;
        g_clear_error(&err);
        g_free(theme);
        if (s.theme != "light" && s.theme != "custom") s.theme = "dark";  // migrate old "system" value

        gchar *bg = g_key_file_get_string(kf, "general", "bg_color", &err);
        if (!err && bg) s.bg_color = bg;
        g_clear_error(&err);
        g_free(bg);

        gint tsize = g_key_file_get_integer(kf, "general", "toolbar_size", &err);
        if (!err && tsize > 0) s.toolbar_size = tsize;
        g_clear_error(&err);

        gchar *fg = g_key_file_get_string(kf, "general", "fg_color", &err);
        if (!err && fg) s.fg_color = fg;
        g_clear_error(&err);
        g_free(fg);

        gint radius = g_key_file_get_integer(kf, "general", "tab_radius", &err);
        if (!err && radius >= 0) s.tab_radius = radius;
        g_clear_error(&err);

        gint theight = g_key_file_get_integer(kf, "general", "tab_height", &err);
        if (!err && theight > 0) s.tab_height = theight;
        g_clear_error(&err);

        gboolean adblock = g_key_file_get_boolean(kf, "general", "adblock_enabled", &err);
        if (!err) s.adblock_enabled = adblock;
        g_clear_error(&err);

        gboolean ram_saving = g_key_file_get_boolean(kf, "general", "ram_saving_mode", &err);
        if (!err) s.ram_saving_mode = ram_saving;
        g_clear_error(&err);
    }

    gsize order_len = 0;
    gchar **order = loaded ? g_key_file_get_string_list(kf, "general", "toolbar_order", &order_len, nullptr) : nullptr;
    if (order) {
        std::vector<std::string> ids;
        for (gsize i = 0; i < order_len; i++) ids.push_back(order[i]);
        g_strfreev(order);
        // keep only known ids, then append any missing ones (covers old/corrupt configs)
        std::vector<std::string> cleaned;
        for (const auto &id : ids) {
            if (std::find(TOOLBAR_IDS.begin(), TOOLBAR_IDS.end(), id) != TOOLBAR_IDS.end() &&
                std::find(cleaned.begin(), cleaned.end(), id) == cleaned.end()) {
                cleaned.push_back(id);
            }
        }
        for (const auto &id : TOOLBAR_IDS) {
            if (std::find(cleaned.begin(), cleaned.end(), id) == cleaned.end()) cleaned.push_back(id);
        }
        s.toolbar_order = cleaned;
    }

    if (loaded) {
        GError *err = nullptr;
        gint pcount = g_key_file_get_integer(kf, "general", "extension_count", &err);
        g_clear_error(&err);
        for (int i = 0; i < pcount; i++) {
            std::string group = "extension:" + std::to_string(i);
            Extension p;
            gchar *v;
            v = g_key_file_get_string(kf, group.c_str(), "name", nullptr);
            p.name = v ? v : "";
            g_free(v);
            v = g_key_file_get_string(kf, group.c_str(), "match", nullptr);
            p.match = v ? v : "";
            g_free(v);
            v = g_key_file_get_string(kf, group.c_str(), "type", nullptr);
            p.type = v ? v : "js";
            g_free(v);
            v = g_key_file_get_string(kf, group.c_str(), "code", nullptr);
            p.code = v ? v : "";
            g_free(v);
            gboolean en = g_key_file_get_boolean(kf, group.c_str(), "enabled", &err);
            p.enabled = err ? true : bool(en);
            g_clear_error(&err);
            s.extensions.push_back(p);
        }
    }

    gsize burl_len = 0, btitle_len = 0;
    gchar **burls = loaded ? g_key_file_get_string_list(kf, "bookmarks", "urls", &burl_len, nullptr) : nullptr;
    gchar **btitles = loaded ? g_key_file_get_string_list(kf, "bookmarks", "titles", &btitle_len, nullptr) : nullptr;
    if (burls) {
        for (gsize i = 0; i < burl_len; i++) {
            Bookmark b;
            b.url = burls[i];
            b.title = (btitles && i < btitle_len) ? btitles[i] : burls[i];
            s.bookmarks.push_back(b);
        }
        g_strfreev(burls);
    }
    if (btitles) g_strfreev(btitles);

    g_key_file_free(kf);
    return s;
}

static void save_settings(const Settings &s) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_boolean(kf, "general", "clear_on_exit", s.clear_on_exit);
    g_key_file_set_string(kf, "general", "language", s.language.c_str());
    g_key_file_set_string(kf, "general", "search_engine", s.search_engine.c_str());
    g_key_file_set_string(kf, "general", "theme", s.theme.c_str());
    g_key_file_set_string(kf, "general", "bg_color", s.bg_color.c_str());
    g_key_file_set_integer(kf, "general", "toolbar_size", s.toolbar_size);
    std::vector<const char *> order_cstrs;
    for (const auto &id : s.toolbar_order) order_cstrs.push_back(id.c_str());
    g_key_file_set_string_list(kf, "general", "toolbar_order", order_cstrs.data(), order_cstrs.size());
    g_key_file_set_string(kf, "general", "fg_color", s.fg_color.c_str());
    g_key_file_set_integer(kf, "general", "tab_radius", s.tab_radius);
    g_key_file_set_integer(kf, "general", "tab_height", s.tab_height);
    g_key_file_set_boolean(kf, "general", "adblock_enabled", s.adblock_enabled);
    g_key_file_set_boolean(kf, "general", "ram_saving_mode", s.ram_saving_mode);

    g_key_file_set_integer(kf, "general", "extension_count", (int)s.extensions.size());
    for (size_t i = 0; i < s.extensions.size(); ++i) {
        std::string group = "extension:" + std::to_string(i);
        const auto &p = s.extensions[i];
        g_key_file_set_string(kf, group.c_str(), "name", p.name.c_str());
        g_key_file_set_string(kf, group.c_str(), "match", p.match.c_str());
        g_key_file_set_string(kf, group.c_str(), "type", p.type.c_str());
        g_key_file_set_string(kf, group.c_str(), "code", p.code.c_str());
        g_key_file_set_boolean(kf, group.c_str(), "enabled", p.enabled);
    }

    std::vector<const char *> burls, btitles;
    for (const auto &b : s.bookmarks) {
        burls.push_back(b.url.c_str());
        btitles.push_back(b.title.c_str());
    }
    g_key_file_set_string_list(kf, "bookmarks", "urls", burls.data(), burls.size());
    g_key_file_set_string_list(kf, "bookmarks", "titles", btitles.data(), btitles.size());

    g_mkdir_with_parents(config_dir().c_str(), 0700);
    GError *err = nullptr;
    if (!g_key_file_save_to_file(kf, config_path().c_str(), &err)) {
        g_warning("Failed to save settings: %s", err ? err->message : "unknown error");
        g_clear_error(&err);
    }
    g_key_file_free(kf);
}

// -- app state ----------------------------------------------------------

struct Tab {
    int id = 0;
    GtkWidget *page = nullptr;      // page widget placed in the content stack
    WebKitWebView *view = nullptr;  // null for internal (settings/cookies/bookmarks) tabs
    Str internal_title = Str::NewTab;  // used only when view == nullptr
};

struct App {
    GtkWidget *window = nullptr;
    GtkWidget *content_stack = nullptr;
    WebKitWebView *chrome_view = nullptr;

    WebKitWebsiteDataManager *data_manager = nullptr;
    WebKitWebContext *web_context = nullptr;

    Settings settings;
    std::vector<Tab *> tabs;
    Tab *current = nullptr;
    int next_tab_id = 1;

    Tab *settings_tab = nullptr;
    Tab *cookies_tab = nullptr;
    Tab *bookmarks_tab = nullptr;
    Tab *extensions_tab = nullptr;

    // Pre-warmed blank tab (WebView already constructed and realized, web
    // process already spawned) waiting to be claimed by the next "+" click
    // -- see prepare_spare_tab(). Not in `tabs`, has no chrome pill yet.
    Tab *spare_tab = nullptr;

    // The bookmarks "..." dropdown is a real native GTK widget floated over
    // the page via GtkOverlay, not HTML drawn inside the chrome WebView --
    // that WebView has a fixed native widget height, so HTML positioned past
    // it is outside the widget's allocation and simply never rendered, no
    // matter what CSS says. A native overlay widget has no such bound: it
    // floats on top of the page content without displacing anything.
    GtkWidget *bookmarks_popup = nullptr;
    GtkWidget *bookmarks_toggle_btn = nullptr;
    GtkWidget *bookmarks_manage_btn = nullptr;
    bool bookmarks_popup_open = false;

    GtkWidget *settings_scroller = nullptr;  // stable container; contents are fully rebuilt on any change
    GtkWidget *settings_status_label = nullptr;

    GtkWidget *cookie_listbox = nullptr;
    GtkWidget *cookie_status_label = nullptr;
    GtkWidget *bookmarks_listbox = nullptr;
    GtkWidget *extensions_listbox = nullptr;

    GtkCssProvider *theme_css = nullptr;

    WebKitUserContentFilterStore *filter_store = nullptr;
    WebKitUserContentFilter *adblock_filter = nullptr;

    std::string initial_uri;              // opened once the chrome finishes loading

    SoupSession *http_session = nullptr;
    GCancellable *suggest_cancellable = nullptr;  // cancels a stale in-flight suggestion request
};

static App *app = nullptr;

// -- forward declarations ------------------------------------------------

static Tab *new_tab(const std::string &uri, bool switch_to, WebKitWebView *related_view);
static void close_tab(Tab *tab);
static Tab *current_tab();
static void switch_to_tab(Tab *tab);
static void update_window_title(Tab *tab);
static void set_address_text(const char *uri);
static void open_settings_tab();
static void open_cookie_editor_tab();
static void open_bookmarks_tab();
static void refresh_language_ui();
static void refresh_settings_content();
static void request_settings_refresh();
static void refresh_cookie_list();
static void refresh_bookmarks_ui();
static void open_cookie_dialog(SoupCookie *original);
static void apply_theme();
static void apply_appearance_css();
static void push_chrome_theme();
static void push_chrome_toolbar();
static void toggle_bookmark_current();
static bool is_bookmarked(const std::string &url);
static void open_bookmarks_popup();
static void close_bookmarks_popup();
static void toggle_bookmarks_popup();
static void run_chrome_js(const std::string &code);
static void on_back(GtkButton *, gpointer);
static void on_forward(GtkButton *, gpointer);
static void on_reload(GtkButton *, gpointer);
static void on_cookies_clicked(GtkButton *, gpointer);
static void open_extensions_tab();
static void refresh_extensions_ui();
static void apply_extensions_to_view(WebKitWebView *view);
static void apply_extensions_to_all_tabs();
static void push_chrome_tab_style();
static void apply_adblock_to_view(WebKitWebView *view);
static void apply_adblock_to_all_tabs();
static void update_chrome_height();
static void fetch_suggestions(const std::string &query);

// -- generic helpers --------------------------------------------------------------

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static const SearchEngine &current_engine() {
    for (const auto &e : SEARCH_ENGINES) {
        if (app->settings.search_engine == e.id) return e;
    }
    return SEARCH_ENGINES[0];
}

static std::string home_uri() { return current_engine().home; }

static std::string to_uri(const std::string &raw) {
    static const std::regex url_re(
        R"(^([a-zA-Z][a-zA-Z0-9+.-]*://))"
        R"(|^localhost(:\d+)?(/.*)?$)"
        R"(|^(\d{1,3}\.){3}\d{1,3}(:\d+)?(/.*)?$)"
        R"(|^[\w-]+(\.[\w-]+)+(:\d+)?(/.*)?$)");
    std::string text = trim(raw);
    if (text.empty()) return home_uri();
    if (std::regex_search(text, url_re)) {
        return text.find("://") != std::string::npos ? text : "https://" + text;
    }
    gchar *escaped = g_uri_escape_string(text.c_str(), nullptr, FALSE);
    std::string result = std::string(current_engine().search_prefix) + escaped;
    g_free(escaped);
    return result;
}

static void set_bold(GtkWidget *label, const char *text) {
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
}

static void clear_container(GtkWidget *container) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

// Connects a signal with a per-connection string argument (freed automatically).
static void connect_with_id(gpointer instance, const char *signal, GCallback cb, const std::string &id) {
    g_signal_connect_data(instance, signal, cb, g_strdup(id.c_str()),
                           (GClosureNotify) + [](gpointer data, GClosure *) { g_free(data); }, GConnectFlags(0));
}

static Tab *find_tab_by_id(int id) {
    for (Tab *t : app->tabs) {
        if (t->id == id) return t;
    }
    return nullptr;
}

// Scales a favicon surface (not owned) into a fresh square surface (caller destroys it).
static cairo_surface_t *scale_favicon_surface(cairo_surface_t *surface, int target) {
    if (!surface) return nullptr;
    int w = cairo_image_surface_get_width(surface);
    int h = cairo_image_surface_get_height(surface);
    if (w <= 0 || h <= 0) return nullptr;
    cairo_surface_t *scaled = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, target, target);
    cairo_t *cr = cairo_create(scaled);
    cairo_scale(cr, double(target) / w, double(target) / h);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    return scaled;
}

// For native GTK favicon displays (bookmark rows).
static void set_favicon_image(GtkImage *image, cairo_surface_t *raw_surface) {
    cairo_surface_t *scaled = scale_favicon_surface(raw_surface, 16);
    if (scaled) {
        gtk_image_set_from_surface(image, scaled);
        cairo_surface_destroy(scaled);
    } else {
        gtk_image_set_from_icon_name(image, "applications-internet-symbolic", GTK_ICON_SIZE_MENU);
    }
}

// For the HTML chrome tab strip: PNG-encodes and base64s a favicon into a data: URL.
static std::string favicon_to_data_url(cairo_surface_t *raw_surface) {
    cairo_surface_t *scaled = scale_favicon_surface(raw_surface, 16);
    if (!scaled) return "";
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(scaled, 0, 0, 16, 16);
    cairo_surface_destroy(scaled);
    if (!pixbuf) return "";
    gchar *buffer = nullptr;
    gsize len = 0;
    bool ok = gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &len, "png", nullptr, nullptr);
    g_object_unref(pixbuf);
    if (!ok) return "";
    gchar *b64 = g_base64_encode(reinterpret_cast<const guchar *>(buffer), len);
    std::string result = std::string("data:image/png;base64,") + b64;
    g_free(b64);
    g_free(buffer);
    return result;
}

// Encodes a C++ string as a double-quoted JS string literal for building evaluate_javascript() calls.
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
    if (!app->chrome_view) return;
    webkit_web_view_evaluate_javascript(app->chrome_view, code.c_str(), -1, nullptr, nullptr, nullptr, nullptr,
                                         nullptr);
}

// Hardware acceleration used to be forced off unconditionally, including
// when "RAM saving mode" was off -- that setting was never actually read
// here. It was justified purely by new-tab-creation speed (neither ALWAYS
// nor ON_DEMAND helped that metric), but the real fix for that turned out
// to be pre-warming a spare tab (see prepare_spare_tab()), not this. Left
// in place, forcing software-only rendering meant every page's scrolling,
// sticky/fixed elements and animations were composited entirely on the
// CPU -- laggy on anything non-trivial (e.g. Google search results). Now
// only actually off when the user opts into RAM saving; ON_DEMAND
// (WebKit's adaptive default) otherwise, so the GPU is used exactly when a
// page needs it for smooth compositing.
//
// Page cache stays disabled: don't hold fully-rendered previous pages in
// memory for instant back/forward (back/forward does a real reload instead).
static void apply_lightweight_settings(WebKitWebView *view) {
    WebKitSettings *settings = webkit_web_view_get_settings(view);
    webkit_settings_set_enable_page_cache(settings, FALSE);
    webkit_settings_set_hardware_acceleration_policy(
        settings, app->settings.ram_saving_mode ? WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER
                                                  : WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND);
}

static void apply_lightweight_settings_to_all_tabs() {
    for (Tab *t : app->tabs) {
        if (t->view) apply_lightweight_settings(t->view);
    }
    if (app->spare_tab && app->spare_tab->view) apply_lightweight_settings(app->spare_tab->view);
    if (app->chrome_view) apply_lightweight_settings(app->chrome_view);
}

// -- tab navigation/state callbacks ---------------------------------------

static void on_title_changed(WebKitWebView *view, GParamSpec *, gpointer user_data) {
    Tab *tab = static_cast<Tab *>(user_data);
    const gchar *title = webkit_web_view_get_title(view);
    std::string text = (title && *title) ? title : tr(Str::NewTab, app->settings.language);
    run_chrome_js("photonUpdateTab(" + std::to_string(tab->id) + "," + js_string_literal(text) + ",null)");
    if (app->current == tab) update_window_title(tab);
}

static void on_uri_changed(WebKitWebView *view, GParamSpec *, gpointer user_data) {
    Tab *tab = static_cast<Tab *>(user_data);
    if (app->current == tab) set_address_text(webkit_web_view_get_uri(view));
}

static void on_favicon_changed(WebKitWebView *view, GParamSpec *, gpointer user_data) {
    Tab *tab = static_cast<Tab *>(user_data);
    std::string data_url = favicon_to_data_url(webkit_web_view_get_favicon(view));
    if (data_url.empty()) return;
    run_chrome_js("photonUpdateTab(" + std::to_string(tab->id) + ",null," + js_string_literal(data_url) + ")");
}

static void set_loading(bool busy) { run_chrome_js(std::string("photonSetLoading(") + (busy ? "true" : "false") + ")"); }

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer user_data) {
    Tab *tab = static_cast<Tab *>(user_data);
    if (app->current != tab) return;
    set_loading(event != WEBKIT_LOAD_FINISHED);
    if (event == WEBKIT_LOAD_COMMITTED) set_address_text(webkit_web_view_get_uri(view));
}

static WebKitWebView *on_create(WebKitWebView *view, WebKitNavigationAction *, gpointer) {
    Tab *tab = new_tab("", true, view);
    return tab->view;
}

// -- tab management ---------------------------------------------------

// Actually constructs the WebView and realizes it into the visible content
// stack -- this is the part that's slow (gtk_stack_add_named onto an
// already-mapped window forces GTK to realize the new WebView immediately,
// which is where WebKit's own per-view setup cost actually lands).
static void finish_new_tab(Tab *tab, const std::string &uri, bool switch_to, WebKitWebView *related_view) {
    GtkWidget *view_widget = related_view ? webkit_web_view_new_with_related_view(related_view)
                                           : webkit_web_view_new_with_context(app->web_context);
    tab->view = WEBKIT_WEB_VIEW(view_widget);
    apply_lightweight_settings(tab->view);
    apply_extensions_to_view(tab->view);
    apply_adblock_to_view(tab->view);

    tab->page = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_container_add(GTK_CONTAINER(tab->page), view_widget);
    gtk_widget_show_all(tab->page);

    g_signal_connect(tab->view, "notify::title", G_CALLBACK(on_title_changed), tab);
    g_signal_connect(tab->view, "notify::uri", G_CALLBACK(on_uri_changed), tab);
    g_signal_connect(tab->view, "notify::favicon", G_CALLBACK(on_favicon_changed), tab);
    g_signal_connect(tab->view, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(tab->view, "create", G_CALLBACK(on_create), tab);

    char name[24];
    snprintf(name, sizeof(name), "tab%d", tab->id);
    gtk_stack_add_named(GTK_STACK(app->content_stack), tab->page, name);

    if (!related_view) webkit_web_view_load_uri(tab->view, uri.empty() ? home_uri().c_str() : uri.c_str());
    if (switch_to) switch_to_tab(tab);
}

struct PendingTabInit {
    int tab_id;
    std::string uri;
    bool switch_to;
};

static void prepare_spare_tab();

static gboolean idle_finish_new_tab(gpointer data) {
    auto *pending = static_cast<PendingTabInit *>(data);
    if (Tab *tab = find_tab_by_id(pending->tab_id)) finish_new_tab(tab, pending->uri, pending->switch_to, nullptr);
    delete pending;
    if (!app->spare_tab) prepare_spare_tab();
    return G_SOURCE_REMOVE;
}

// Keeps one blank tab fully constructed and realized (WebView created, web
// process already spawned by loading about:blank) sitting off-screen, so a
// "+" click can claim it and switch instantly instead of paying the
// construction/realize cost -- which is the actual slow part, see
// finish_new_tab() -- on the click itself. Refilled after every claim.
static void prepare_spare_tab() {
    if (app->spare_tab) return;
    Tab *tab = new Tab();
    tab->id = app->next_tab_id++;
    finish_new_tab(tab, "about:blank", false, nullptr);
    app->spare_tab = tab;
}

static gboolean idle_prepare_spare_tab(gpointer) {
    prepare_spare_tab();
    return G_SOURCE_REMOVE;
}

static Tab *new_tab(const std::string &uri, bool switch_to, WebKitWebView *related_view) {
    if (!related_view && app->spare_tab) {
        Tab *tab = app->spare_tab;
        app->spare_tab = nullptr;
        app->tabs.push_back(tab);
        run_chrome_js("photonAddTab(" + std::to_string(tab->id) + "," +
                      js_string_literal(tr(Str::NewTab, app->settings.language)) + ",null,\"\")");
        webkit_web_view_load_uri(tab->view, uri.empty() ? home_uri().c_str() : uri.c_str());
        if (switch_to) switch_to_tab(tab);
        g_idle_add(idle_prepare_spare_tab, nullptr);
        return tab;
    }

    Tab *tab = new Tab();
    tab->id = app->next_tab_id++;

    // Add the tab pill first, before creating the WebView: constructing a new
    // WebKitWebView is the slow part. Queuing the pill's JS call before that
    // work starts gives the chrome a chance to render it immediately instead
    // of appearing to stall until the new view is up.
    app->tabs.push_back(tab);
    run_chrome_js("photonAddTab(" + std::to_string(tab->id) + "," +
                  js_string_literal(tr(Str::NewTab, app->settings.language)) + ",null,\"\")");

    if (related_view) {
        // window.open()/popups: WebKit's "create" signal needs a real, usable
        // WebView returned synchronously, so this path can't be deferred.
        finish_new_tab(tab, uri, switch_to, related_view);
    } else {
        // Regular new tab: defer the actual WebView construction/realize to
        // the next idle cycle, so the pill gets an actual frame to render on
        // screen before that work runs, instead of both happening in the
        // same blocking call.
        g_idle_add(idle_finish_new_tab, new PendingTabInit{tab->id, uri, switch_to});
    }
    return tab;
}

static void close_tab(Tab *tab) {
    if (tab == app->settings_tab) {
        app->settings_tab = nullptr;
        app->settings_scroller = nullptr;
        app->settings_status_label = nullptr;
    }
    if (tab == app->cookies_tab) {
        app->cookies_tab = nullptr;
        app->cookie_listbox = nullptr;
        app->cookie_status_label = nullptr;
    }
    if (tab == app->bookmarks_tab) {
        app->bookmarks_tab = nullptr;
        app->bookmarks_listbox = nullptr;
    }
    if (tab == app->extensions_tab) {
        app->extensions_tab = nullptr;
        app->extensions_listbox = nullptr;
    }

    run_chrome_js("photonRemoveTab(" + std::to_string(tab->id) + ")");

    auto it = std::find(app->tabs.begin(), app->tabs.end(), tab);
    if (it != app->tabs.end()) app->tabs.erase(it);
    bool was_current = (app->current == tab);
    // Clear this before deleting tab, not after: if was_current and the next
    // switch_to_tab() call below no-ops (its target is itself still pending,
    // page == nullptr), app->current must not be left dangling at freed
    // memory in the meantime -- nullptr is safe, every reader already
    // guards for it.
    if (was_current) app->current = nullptr;

    // tab->page can still be null if this tab's finish_new_tab() hasn't run
    // yet (deferred via idle); its pending callback looks tabs up by id, so
    // deleting the Tab here is enough to make it safely no-op when it fires.
    if (tab->page) gtk_widget_destroy(tab->page);
    delete tab;

    if (app->tabs.empty()) {
        gtk_main_quit();
        return;
    }
    if (was_current) switch_to_tab(app->tabs.back());
}

static Tab *current_tab() { return app->current; }

static void switch_to_tab(Tab *tab) {
    // tab->page is briefly null for a newly created tab awaiting its
    // deferred finish_new_tab(); finish_new_tab() will call switch_to_tab()
    // itself once that's done if switch_to was requested.
    if (!tab || !tab->page) return;
    close_bookmarks_popup();
    app->current = tab;
    gtk_stack_set_visible_child(GTK_STACK(app->content_stack), tab->page);
    update_window_title(tab);

    const gchar *uri = tab->view ? webkit_web_view_get_uri(tab->view) : nullptr;
    std::string address = uri ? uri : "";
    run_chrome_js("photonActivateTab(" + std::to_string(tab->id) + "," + js_string_literal(address) + ")");
}

static void update_window_title(Tab *tab) {
    std::string title = "Photon Browser";
    if (tab) {
        if (tab->view) {
            const gchar *t = webkit_web_view_get_title(tab->view);
            if (t && *t) title = std::string(t) + " — Photon Browser";
        } else {
            title = std::string(tr(tab->internal_title, app->settings.language)) + " — Photon Browser";
        }
    }
    gtk_window_set_title(GTK_WINDOW(app->window), title.c_str());
}

static void set_address_text(const char *uri) {
    run_chrome_js("photonSetAddress(" + js_string_literal(uri ? uri : "") + ")");
}

// -- address bar search suggestions -----------------------------------------
//
// Fetched from native code with libsoup rather than via fetch() in the
// chrome's own JS: the chrome page has no real origin (loaded via
// load_html with no base URI), so a cross-origin fetch from there would be
// at the mercy of the target's CORS headers. A plain native HTTP request
// has no such restriction.

// Both DuckDuckGo and Google (Firefox-client format) return the same shape:
// ["query", ["s1", "s2", ...], ...]. Verified against the live endpoints
// rather than assumed -- DuckDuckGo's is not the {"phrase":...} object-list
// format some of its other endpoints use.
static std::vector<std::string> parse_suggestions(const std::string &, const std::string &body) {
    std::vector<std::string> out;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body.c_str(), (gssize)body.size(), nullptr)) {
        g_object_unref(parser);
        return out;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!root || JSON_NODE_TYPE(root) != JSON_NODE_ARRAY) {
        g_object_unref(parser);
        return out;
    }
    JsonArray *top = json_node_get_array(root);
    if (json_array_get_length(top) >= 2) {
        JsonNode *listNode = json_array_get_element(top, 1);
        if (JSON_NODE_TYPE(listNode) == JSON_NODE_ARRAY) {
            JsonArray *list = json_node_get_array(listNode);
            for (guint i = 0; i < json_array_get_length(list) && out.size() < 8; i++) {
                JsonNode *el = json_array_get_element(list, i);
                if (JSON_NODE_TYPE(el) == JSON_NODE_VALUE) out.push_back(json_node_get_string(el));
            }
        }
    }
    g_object_unref(parser);
    return out;
}

static void on_suggest_response(GObject *source, GAsyncResult *res, gpointer user_data) {
    std::string *engine_id = static_cast<std::string *>(user_data);
    GError *err = nullptr;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &err);
    if (!bytes) {
        g_clear_error(&err);  // covers cancellation (a newer request superseded this one) and network errors alike
        delete engine_id;
        return;
    }
    gsize len = 0;
    const char *data = static_cast<const char *>(g_bytes_get_data(bytes, &len));
    std::vector<std::string> suggestions = parse_suggestions(*engine_id, std::string(data, len));
    g_bytes_unref(bytes);
    delete engine_id;

    std::string arr = "[";
    for (size_t i = 0; i < suggestions.size(); ++i) {
        if (i) arr += ",";
        arr += js_string_literal(suggestions[i]);
    }
    arr += "]";
    run_chrome_js("photonShowSuggestions(" + arr + ")");
}

static void fetch_suggestions(const std::string &query) {
    if (query.empty()) return;
    if (app->suggest_cancellable) {
        g_cancellable_cancel(app->suggest_cancellable);
        g_object_unref(app->suggest_cancellable);
    }
    app->suggest_cancellable = g_cancellable_new();

    gchar *encoded = g_uri_escape_string(query.c_str(), nullptr, FALSE);
    std::string url = std::string(current_engine().suggest_prefix) + encoded;
    g_free(encoded);

    SoupMessage *msg = soup_message_new("GET", url.c_str());
    if (!msg) return;
    auto *engine_id = new std::string(current_engine().id);
    soup_session_send_and_read_async(app->http_session, msg, G_PRIORITY_DEFAULT, app->suggest_cancellable,
                                      on_suggest_response, engine_id);
    g_object_unref(msg);
}

// -- navigation actions ----------------------------------------------------

static void navigate_current(const std::string &raw_text) {
    std::string target = to_uri(raw_text);
    Tab *tab = current_tab();
    if (tab && tab->view) {
        webkit_web_view_load_uri(tab->view, target.c_str());
    } else {
        new_tab(target, true, nullptr);
    }
}

static void on_back(GtkButton *, gpointer) {
    Tab *tab = current_tab();
    if (tab && tab->view && webkit_web_view_can_go_back(tab->view)) webkit_web_view_go_back(tab->view);
}

static void on_forward(GtkButton *, gpointer) {
    Tab *tab = current_tab();
    if (tab && tab->view && webkit_web_view_can_go_forward(tab->view)) webkit_web_view_go_forward(tab->view);
}

static void on_reload(GtkButton *, gpointer) {
    Tab *tab = current_tab();
    if (!tab || !tab->view) return;
    if (webkit_web_view_is_loading(tab->view)) {
        webkit_web_view_stop_loading(tab->view);
    } else {
        webkit_web_view_reload(tab->view);
    }
}

// -- chrome message bridge (JS -> native) -----------------------------------

static void on_chrome_message(WebKitUserContentManager *, WebKitJavascriptResult *js_result, gpointer) {
    JSCValue *value = webkit_javascript_result_get_js_value(js_result);
    gchar *raw = jsc_value_to_string(value);
    std::string msg = raw ? raw : "";
    g_free(raw);

    auto starts_with = [&](const char *prefix) { return msg.rfind(prefix, 0) == 0; };

    if (msg == "back") {
        on_back(nullptr, nullptr);
    } else if (msg == "forward") {
        on_forward(nullptr, nullptr);
    } else if (msg == "reload") {
        on_reload(nullptr, nullptr);
    } else if (msg == "newTab") {
        new_tab("", true, nullptr);
    } else if (msg == "openSettings") {
        open_settings_tab();
    } else if (msg == "bmMenuToggle") {
        toggle_bookmarks_popup();
    } else if (msg == "bmMenuClose") {
        close_bookmarks_popup();
    } else if (starts_with("switchTab:")) {
        if (Tab *t = find_tab_by_id(atoi(msg.c_str() + 10))) switch_to_tab(t);
    } else if (starts_with("closeTab:")) {
        if (Tab *t = find_tab_by_id(atoi(msg.c_str() + 9))) close_tab(t);
    } else if (starts_with("navigate:")) {
        gchar *decoded = g_uri_unescape_string(msg.c_str() + 9, nullptr);
        navigate_current(decoded ? decoded : "");
        g_free(decoded);
    } else if (starts_with("suggest:")) {
        gchar *decoded = g_uri_unescape_string(msg.c_str() + 8, nullptr);
        fetch_suggestions(decoded ? decoded : "");
        g_free(decoded);
    } else if (msg == "winMinimize") {
        gtk_window_iconify(GTK_WINDOW(app->window));
    } else if (msg == "winMaximize") {
        if (gtk_window_is_maximized(GTK_WINDOW(app->window))) {
            gtk_window_unmaximize(GTK_WINDOW(app->window));
        } else {
            gtk_window_maximize(GTK_WINDOW(app->window));
        }
    } else if (msg == "winClose") {
        gtk_window_close(GTK_WINDOW(app->window));
    } else if (starts_with("winDrag:")) {
        int sx = 0, sy = 0;
        if (sscanf(msg.c_str() + 8, "%d,%d", &sx, &sy) == 2) {
            gtk_window_begin_move_drag(GTK_WINDOW(app->window), 1, sx, sy, gtk_get_current_event_time());
        }
    }
}

// -- data clearing --------------------------------------------------------

static void on_clear_now_done(GObject *source, GAsyncResult *res, gpointer) {
    webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), res, nullptr);
    if (app->settings_status_label) {
        gtk_label_set_text(GTK_LABEL(app->settings_status_label), tr(Str::ClearDone, app->settings.language));
    }
}

static void on_clear_now_clicked(GtkButton *, gpointer) {
    if (app->settings_status_label) gtk_label_set_text(GTK_LABEL(app->settings_status_label), "");
    webkit_website_data_manager_clear(app->data_manager, WEBKIT_WEBSITE_DATA_ALL, 0, nullptr, on_clear_now_done,
                                       nullptr);
}

static void on_exit_clear_done(GObject *source, GAsyncResult *res, gpointer) {
    webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), res, nullptr);
    gtk_widget_destroy(app->window);
}

static gboolean on_delete_event(GtkWidget *, GdkEvent *, gpointer) {
    if (app->settings.clear_on_exit) {
        webkit_website_data_manager_clear(app->data_manager, WEBKIT_WEBSITE_DATA_ALL, 0, nullptr,
                                           on_exit_clear_done, nullptr);
        return TRUE;  // block the default close until clearing finishes
    }
    return FALSE;
}

// -- bookmarks ----------------------------------------------------------

static bool is_bookmarked(const std::string &url) {
    for (const auto &b : app->settings.bookmarks) {
        if (b.url == url) return true;
    }
    return false;
}

// Floats over the page via GtkOverlay -- see the App::bookmarks_popup
// comment. Its label is computed fresh each time it opens rather than kept
// continuously in sync, since nothing needs to observe it while it's closed.
static void open_bookmarks_popup() {
    Tab *tab = app->current;
    bool marked = false;
    if (tab && tab->view) {
        const gchar *uri = webkit_web_view_get_uri(tab->view);
        if (uri) marked = is_bookmarked(uri);
    }
    gtk_button_set_label(GTK_BUTTON(app->bookmarks_toggle_btn),
                          tr(marked ? Str::RemoveBookmark : Str::BookmarkThis, app->settings.language));
    app->bookmarks_popup_open = true;
    gtk_widget_show(app->bookmarks_popup);
}

static void close_bookmarks_popup() {
    if (!app->bookmarks_popup || !app->bookmarks_popup_open) return;
    app->bookmarks_popup_open = false;
    gtk_widget_hide(app->bookmarks_popup);
}

static void toggle_bookmarks_popup() {
    if (app->bookmarks_popup_open) close_bookmarks_popup(); else open_bookmarks_popup();
}

static void toggle_bookmark_current() {
    Tab *tab = current_tab();
    if (!tab || !tab->view) return;
    const gchar *uri = webkit_web_view_get_uri(tab->view);
    if (!uri || !*uri) return;
    std::string url = uri;
    auto &bms = app->settings.bookmarks;
    auto it = std::find_if(bms.begin(), bms.end(), [&](const Bookmark &b) { return b.url == url; });
    if (it != bms.end()) {
        bms.erase(it);
    } else {
        const gchar *title = webkit_web_view_get_title(tab->view);
        Bookmark b;
        b.url = url;
        b.title = (title && *title) ? title : url;
        bms.push_back(b);
    }
    save_settings(app->settings);
    refresh_bookmarks_ui();
}

static void on_bookmarks_popup_toggle_clicked(GtkButton *, gpointer) {
    close_bookmarks_popup();
    toggle_bookmark_current();
}

static void on_bookmarks_popup_manage_clicked(GtkButton *, gpointer) {
    close_bookmarks_popup();
    open_bookmarks_tab();
}

// Left-align a GtkButton's label -- gtk_button_set_alignment() was removed
// in GTK 3.20; the label's own xalign is the replacement.
static void left_align_button_label(GtkWidget *button) {
    gtk_label_set_xalign(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), 0.0f);
}

static GtkWidget *build_bookmarks_popup() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "bookmarks-popup");
    gtk_widget_set_size_request(box, 190, -1);
    gtk_widget_set_margin_end(box, 6);

    app->bookmarks_toggle_btn = gtk_button_new_with_label(tr(Str::BookmarkThis, app->settings.language));
    left_align_button_label(app->bookmarks_toggle_btn);
    g_signal_connect(app->bookmarks_toggle_btn, "clicked", G_CALLBACK(on_bookmarks_popup_toggle_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(box), app->bookmarks_toggle_btn, FALSE, FALSE, 0);

    app->bookmarks_manage_btn = gtk_button_new_with_label(tr(Str::ManageBookmarks, app->settings.language));
    left_align_button_label(app->bookmarks_manage_btn);
    g_signal_connect(app->bookmarks_manage_btn, "clicked", G_CALLBACK(on_bookmarks_popup_manage_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(box), app->bookmarks_manage_btn, FALSE, FALSE, 0);

    // Mark children visible now so a plain gtk_widget_show(box) later (no
    // recursion) reveals them too, but exclude the box itself from the
    // window's gtk_widget_show_all() -- it must stay hidden until toggled.
    gtk_widget_show_all(box);
    gtk_widget_set_no_show_all(box, TRUE);
    gtk_widget_hide(box);
    return box;
}

static void on_bookmark_favicon_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    GtkWidget *icon = GTK_WIDGET(user_data);
    cairo_surface_t *surface = webkit_favicon_database_get_favicon_finish(WEBKIT_FAVICON_DATABASE(source), res, nullptr);
    if (surface) {
        set_favicon_image(GTK_IMAGE(icon), surface);
        cairo_surface_destroy(surface);
    }
    g_object_unref(icon);
}

static void on_bookmark_row_open(GtkListBox *, GtkListBoxRow *row, gpointer) {
    const char *url = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "url"));
    if (url) new_tab(url, true, nullptr);
}

static void on_bookmark_delete_clicked(GtkButton *, gpointer user_data) {
    std::string url = static_cast<const char *>(user_data);
    auto &bms = app->settings.bookmarks;
    bms.erase(std::remove_if(bms.begin(), bms.end(), [&](const Bookmark &b) { return b.url == url; }), bms.end());
    save_settings(app->settings);
    refresh_bookmarks_ui();
}

static void refresh_bookmarks_ui() {
    if (!app->bookmarks_listbox) return;
    clear_container(app->bookmarks_listbox);
    WebKitFaviconDatabase *db = webkit_web_context_get_favicon_database(app->web_context);
    for (const auto &bm : app->settings.bookmarks) {
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data_full(G_OBJECT(row), "url", g_strdup(bm.url.c_str()), g_free);

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(hbox), 6);

        GtkWidget *icon = gtk_image_new_from_icon_name("applications-internet-symbolic", GTK_ICON_SIZE_MENU);

        GtkWidget *title_lbl = gtk_label_new(bm.title.c_str());
        gtk_label_set_width_chars(GTK_LABEL(title_lbl), 22);
        gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0);

        GtkWidget *url_lbl = gtk_label_new(bm.url.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(url_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(url_lbl), 0.0);
        gtk_widget_set_hexpand(url_lbl, TRUE);

        GtkWidget *delete_btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(delete_btn), GTK_RELIEF_NONE);
        gtk_button_set_image(GTK_BUTTON(delete_btn),
                              gtk_image_new_from_icon_name("edit-delete-symbolic", GTK_ICON_SIZE_MENU));
        connect_with_id(delete_btn, "clicked", G_CALLBACK(on_bookmark_delete_clicked), bm.url);

        gtk_box_pack_start(GTK_BOX(hbox), icon, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), title_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), url_lbl, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), delete_btn, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_list_box_insert(GTK_LIST_BOX(app->bookmarks_listbox), row, -1);

        g_object_ref(icon);
        webkit_favicon_database_get_favicon(db, bm.url.c_str(), nullptr, on_bookmark_favicon_ready, icon);
    }
    gtk_widget_show_all(app->bookmarks_listbox);
}

static GtkWidget *build_bookmarks_page() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(scroller, TRUE);
    app->bookmarks_listbox = gtk_list_box_new();
    g_signal_connect(app->bookmarks_listbox, "row-activated", G_CALLBACK(on_bookmark_row_open), nullptr);
    gtk_container_add(GTK_CONTAINER(scroller), app->bookmarks_listbox);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);
    return box;
}

static void open_bookmarks_tab() {
    if (app->bookmarks_tab) {
        switch_to_tab(app->bookmarks_tab);
        return;
    }
    Tab *tab = new Tab();
    tab->id = app->next_tab_id++;
    tab->internal_title = Str::Bookmarks;
    tab->page = build_bookmarks_page();
    gtk_widget_show_all(tab->page);

    char name[24];
    snprintf(name, sizeof(name), "tab%d", tab->id);
    gtk_stack_add_named(GTK_STACK(app->content_stack), tab->page, name);

    app->tabs.push_back(tab);
    app->bookmarks_tab = tab;
    run_chrome_js("photonAddTab(" + std::to_string(tab->id) + "," +
                  js_string_literal(tr(Str::Bookmarks, app->settings.language)) + ",null,\"\\u2605\")");
    refresh_bookmarks_ui();
    switch_to_tab(tab);
}

// -- cookie editor ----------------------------------------------------------

static void on_cookie_generic_done(GObject *, GAsyncResult *, gpointer) { refresh_cookie_list(); }

static void on_cookie_edit_clicked(GtkButton *, gpointer user_data) {
    open_cookie_dialog(static_cast<SoupCookie *>(user_data));
}

static void on_cookie_delete_clicked(GtkButton *, gpointer user_data) {
    SoupCookie *c = static_cast<SoupCookie *>(user_data);
    WebKitCookieManager *cm = webkit_website_data_manager_get_cookie_manager(app->data_manager);
    webkit_cookie_manager_delete_cookie(cm, c, nullptr, on_cookie_generic_done, nullptr);
}

static void on_get_all_cookies_done(GObject *source, GAsyncResult *res, gpointer) {
    GList *cookies = webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(source), res, nullptr);
    if (!app->cookie_listbox) {
        if (cookies) g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
        return;
    }
    clear_container(app->cookie_listbox);
    if (app->cookie_status_label) {
        std::string text = std::to_string(g_list_length(cookies)) + " " + tr(Str::CookiesWord, app->settings.language);
        gtk_label_set_text(GTK_LABEL(app->cookie_status_label), text.c_str());
    }
    for (GList *l = cookies; l; l = l->next) {
        SoupCookie *c = static_cast<SoupCookie *>(l->data);  // ownership transfers to the row below

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(hbox), 6);

        GtkWidget *domain_lbl = gtk_label_new(soup_cookie_get_domain(c));
        gtk_label_set_width_chars(GTK_LABEL(domain_lbl), 20);
        gtk_label_set_ellipsize(GTK_LABEL(domain_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(domain_lbl), 0.0);

        GtkWidget *name_lbl = gtk_label_new(soup_cookie_get_name(c));
        gtk_label_set_width_chars(GTK_LABEL(name_lbl), 16);
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);

        GtkWidget *value_lbl = gtk_label_new(soup_cookie_get_value(c));
        gtk_label_set_ellipsize(GTK_LABEL(value_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(value_lbl), 0.0);
        gtk_widget_set_hexpand(value_lbl, TRUE);

        GtkWidget *edit_btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(edit_btn), GTK_RELIEF_NONE);
        gtk_button_set_image(GTK_BUTTON(edit_btn),
                              gtk_image_new_from_icon_name("document-edit-symbolic", GTK_ICON_SIZE_MENU));

        GtkWidget *delete_btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(delete_btn), GTK_RELIEF_NONE);
        gtk_button_set_image(GTK_BUTTON(delete_btn),
                              gtk_image_new_from_icon_name("edit-delete-symbolic", GTK_ICON_SIZE_MENU));

        gtk_box_pack_start(GTK_BOX(hbox), domain_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), name_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), value_lbl, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), edit_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), delete_btn, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_list_box_insert(GTK_LIST_BOX(app->cookie_listbox), row, -1);

        g_object_set_data_full(G_OBJECT(row), "cookie", c, (GDestroyNotify)soup_cookie_free);
        g_signal_connect(edit_btn, "clicked", G_CALLBACK(on_cookie_edit_clicked), c);
        g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_cookie_delete_clicked), c);
    }
    gtk_widget_show_all(app->cookie_listbox);
    g_list_free(cookies);  // elements are now owned by the rows
}

static void refresh_cookie_list() {
    WebKitCookieManager *cm = webkit_website_data_manager_get_cookie_manager(app->data_manager);
    webkit_cookie_manager_get_all_cookies(cm, nullptr, on_get_all_cookies_done, nullptr);
}

struct CookieDialogCtx {
    GtkWidget *name_entry;
    GtkWidget *value_entry;
    GtkWidget *domain_entry;
    GtkWidget *path_entry;
    SoupCookie *original;
};

static void on_cookie_dialog_response(GtkDialog *dialog, gint response, gpointer user_data) {
    auto *ctx = static_cast<CookieDialogCtx *>(user_data);
    if (response == GTK_RESPONSE_OK) {
        std::string name = gtk_entry_get_text(GTK_ENTRY(ctx->name_entry));
        std::string value = gtk_entry_get_text(GTK_ENTRY(ctx->value_entry));
        std::string domain = gtk_entry_get_text(GTK_ENTRY(ctx->domain_entry));
        std::string path = gtk_entry_get_text(GTK_ENTRY(ctx->path_entry));
        if (!name.empty() && !domain.empty()) {
            WebKitCookieManager *cm = webkit_website_data_manager_get_cookie_manager(app->data_manager);
            if (ctx->original) webkit_cookie_manager_delete_cookie(cm, ctx->original, nullptr, on_cookie_generic_done, nullptr);
            SoupCookie *nc = soup_cookie_new(name.c_str(), value.c_str(), domain.c_str(),
                                              path.empty() ? "/" : path.c_str(), SOUP_COOKIE_MAX_AGE_ONE_YEAR);
            webkit_cookie_manager_add_cookie(cm, nc, nullptr, on_cookie_generic_done, nullptr);
            soup_cookie_free(nc);
        }
    }
    if (ctx->original) soup_cookie_free(ctx->original);
    delete ctx;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void open_cookie_dialog(SoupCookie *original) {
    const std::string &lang = app->settings.language;
    GtkWidget *dialog =
        gtk_dialog_new_with_buttons(original ? tr(Str::EditCookie, lang) : tr(Str::AddCookie, lang),
                                     GTK_WINDOW(app->window), GTK_DIALOG_MODAL, tr(Str::Cancel, lang),
                                     GTK_RESPONSE_CANCEL, tr(Str::Save, lang), GTK_RESPONSE_OK, nullptr);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    auto add_row = [&](int row, const char *label_text, const char *value) {
        GtkWidget *label = gtk_label_new(label_text);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0);
        GtkWidget *entry = gtk_entry_new();
        if (value) gtk_entry_set_text(GTK_ENTRY(entry), value);
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        return entry;
    };

    auto *ctx = new CookieDialogCtx();
    ctx->name_entry = add_row(0, tr(Str::NameField, lang), original ? soup_cookie_get_name(original) : nullptr);
    ctx->value_entry = add_row(1, tr(Str::ValueField, lang), original ? soup_cookie_get_value(original) : nullptr);
    ctx->domain_entry = add_row(2, tr(Str::Domain, lang), original ? soup_cookie_get_domain(original) : nullptr);
    ctx->path_entry = add_row(3, tr(Str::PathField, lang), original ? soup_cookie_get_path(original) : "/");
    ctx->original = original ? soup_cookie_copy(original) : nullptr;

    gtk_container_add(GTK_CONTAINER(content), grid);
    gtk_widget_show_all(dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(on_cookie_dialog_response), ctx);
}

static void on_add_cookie_clicked(GtkButton *, gpointer) { open_cookie_dialog(nullptr); }
static void on_refresh_cookies_clicked(GtkButton *, gpointer) { refresh_cookie_list(); }
static void on_delete_all_cookies_done(GObject *, GAsyncResult *, gpointer) { refresh_cookie_list(); }
static void on_delete_all_cookies_clicked(GtkButton *, gpointer) {
    webkit_website_data_manager_clear(app->data_manager, WEBKIT_WEBSITE_DATA_COOKIES, 0, nullptr,
                                       on_delete_all_cookies_done, nullptr);
}

static GtkWidget *build_cookie_editor_page() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    const std::string &lang = app->settings.language;

    GtkWidget *header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add_btn = gtk_button_new_with_label(tr(Str::AddCookie, lang));
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_cookie_clicked), nullptr);
    GtkWidget *refresh_btn = gtk_button_new_with_label(tr(Str::RefreshCookies, lang));
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_cookies_clicked), nullptr);
    GtkWidget *delete_all_btn = gtk_button_new_with_label(tr(Str::DeleteAllCookies, lang));
    g_signal_connect(delete_all_btn, "clicked", G_CALLBACK(on_delete_all_cookies_clicked), nullptr);
    app->cookie_status_label = gtk_label_new("");

    gtk_box_pack_start(GTK_BOX(header_row), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_row), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_row), delete_all_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header_row), app->cookie_status_label, FALSE, FALSE, 12);
    gtk_box_pack_start(GTK_BOX(box), header_row, FALSE, FALSE, 0);

    GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(scroller, TRUE);
    app->cookie_listbox = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroller), app->cookie_listbox);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);

    return box;
}

static void open_cookie_editor_tab() {
    if (app->cookies_tab) {
        switch_to_tab(app->cookies_tab);
        return;
    }
    Tab *tab = new Tab();
    tab->id = app->next_tab_id++;
    tab->internal_title = Str::Cookies;
    tab->page = build_cookie_editor_page();
    gtk_widget_show_all(tab->page);

    char name[24];
    snprintf(name, sizeof(name), "tab%d", tab->id);
    gtk_stack_add_named(GTK_STACK(app->content_stack), tab->page, name);

    app->tabs.push_back(tab);
    app->cookies_tab = tab;
    run_chrome_js("photonAddTab(" + std::to_string(tab->id) + "," +
                  js_string_literal(tr(Str::Cookies, app->settings.language)) + ",null,\"\\u25c6\")");
    refresh_cookie_list();
    switch_to_tab(tab);
}

// -- adblocker ----------------------------------------------------------
//
// Uses WebKit's native content-blocker engine (WebKitUserContentFilterStore),
// the same mechanism Safari's content blockers use -- rules are compiled
// once into an efficient matcher and applied per-tab, rather than injecting
// blocking JS. This is a curated list of common ad/tracker domains, not a
// full EasyList-equivalent: good for the common case, not exhaustive.

static const char *ADBLOCK_RULES_JSON = R"JSON([
  {"trigger":{"url-filter":"doubleclick\\.net"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"googlesyndication\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"googletagservices\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"googletagmanager\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"google-analytics\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"googleadservices\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"adservice\\.google\\."},"action":{"type":"block"}},
  {"trigger":{"url-filter":"adnxs\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"amazon-adsystem\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"scorecardresearch\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"facebook\\.com/tr"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"connect\\.facebook\\.net"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"taboola\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"outbrain\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"criteo\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"adsrvr\\.org"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"pubmatic\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"rubiconproject\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"moatads\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"quantserve\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"hotjar\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"mixpanel\\.com"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"segment\\.(io|com)"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"mc\\.yandex\\.(ru|com)"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"adform\\.net"},"action":{"type":"block"}},
  {"trigger":{"url-filter":"adroll\\.com"},"action":{"type":"block"}}
])JSON";

static void apply_adblock_to_view(WebKitWebView *view) {
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(view);
    webkit_user_content_manager_remove_all_filters(ucm);
    if (app->settings.adblock_enabled && app->adblock_filter) {
        webkit_user_content_manager_add_filter(ucm, app->adblock_filter);
    }
}

static void apply_adblock_to_all_tabs() {
    for (Tab *t : app->tabs) {
        if (t->view) apply_adblock_to_view(t->view);
    }
    if (app->spare_tab && app->spare_tab->view) apply_adblock_to_view(app->spare_tab->view);
}

static void on_adblock_filter_ready(GObject *source, GAsyncResult *res, gpointer) {
    GError *err = nullptr;
    WebKitUserContentFilter *filter =
        webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &err);
    if (!filter) {
        g_warning("Adblock filter compile failed: %s", err ? err->message : "unknown error");
        g_clear_error(&err);
        return;
    }
    app->adblock_filter = filter;
    apply_adblock_to_all_tabs();
}

static void compile_adblock_filter() {
    std::string dir = std::string(g_get_home_dir()) + "/.local/share/photon-browser/filters";
    g_mkdir_with_parents(dir.c_str(), 0700);
    app->filter_store = webkit_user_content_filter_store_new(dir.c_str());
    GBytes *bytes = g_bytes_new_static(ADBLOCK_RULES_JSON, strlen(ADBLOCK_RULES_JSON));
    webkit_user_content_filter_store_save(app->filter_store, "photon-adblock", bytes, nullptr,
                                           on_adblock_filter_ready, nullptr);
    g_bytes_unref(bytes);
}

static void on_adblock_toggled(GtkToggleButton *btn, gpointer) {
    app->settings.adblock_enabled = gtk_toggle_button_get_active(btn);
    save_settings(app->settings);
    apply_adblock_to_all_tabs();
}

static void on_ram_saving_toggled(GtkToggleButton *btn, gpointer) {
    app->settings.ram_saving_mode = gtk_toggle_button_get_active(btn);
    save_settings(app->settings);
    apply_lightweight_settings_to_all_tabs();
}

// -- extensions (userscripts / userstyles) --------------------------------
//
// WebKitGTK has no public API for loading real Chrome/Firefox-format browser
// extensions (no extension-store hookup is possible), so this is a
// Tampermonkey/Stylus-style manager instead: JS or CSS snippets injected
// into pages via WebKitUserContentManager, optionally scoped to a URL match
// pattern. Applied per-tab at creation and re-applied to all open tabs
// whenever the extension list changes.

static void apply_extensions_to_view(WebKitWebView *view) {
    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(view);
    webkit_user_content_manager_remove_all_scripts(ucm);
    webkit_user_content_manager_remove_all_style_sheets(ucm);
    for (const auto &p : app->settings.extensions) {
        if (!p.enabled || p.code.empty()) continue;
        const gchar *allow_list[2] = {nullptr, nullptr};
        const gchar *const *allow = nullptr;
        if (!p.match.empty()) {
            allow_list[0] = p.match.c_str();
            allow = allow_list;
        }
        if (p.type == "css") {
            WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new(
                p.code.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, allow, nullptr);
            webkit_user_content_manager_add_style_sheet(ucm, sheet);
            webkit_user_style_sheet_unref(sheet);
        } else {
            WebKitUserScript *script =
                webkit_user_script_new(p.code.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, allow, nullptr);
            webkit_user_content_manager_add_script(ucm, script);
            webkit_user_script_unref(script);
        }
    }
}

static void apply_extensions_to_all_tabs() {
    for (Tab *t : app->tabs) {
        if (t->view) apply_extensions_to_view(t->view);
    }
    if (app->spare_tab && app->spare_tab->view) apply_extensions_to_view(app->spare_tab->view);
}

static void on_extension_enabled_toggled(GtkToggleButton *btn, gpointer user_data) {
    int idx = GPOINTER_TO_INT(user_data);
    if (idx < 0 || idx >= int(app->settings.extensions.size())) return;
    app->settings.extensions[idx].enabled = gtk_toggle_button_get_active(btn);
    save_settings(app->settings);
    apply_extensions_to_all_tabs();
}

static void on_extension_delete_clicked(GtkButton *, gpointer user_data) {
    int idx = GPOINTER_TO_INT(user_data);
    if (idx < 0 || idx >= int(app->settings.extensions.size())) return;
    app->settings.extensions.erase(app->settings.extensions.begin() + idx);
    save_settings(app->settings);
    apply_extensions_to_all_tabs();
    refresh_extensions_ui();
}

struct ExtensionDialogCtx {
    int index;  // -1 for a new extension
    GtkWidget *name_entry;
    GtkWidget *match_entry;
    GtkWidget *type_combo;
    GtkWidget *code_view;
    GtkWidget *enabled_check;
};

static void on_extension_dialog_response(GtkDialog *dialog, gint response, gpointer user_data) {
    auto *ctx = static_cast<ExtensionDialogCtx *>(user_data);
    if (response == GTK_RESPONSE_OK) {
        Extension p;
        p.name = gtk_entry_get_text(GTK_ENTRY(ctx->name_entry));
        p.match = gtk_entry_get_text(GTK_ENTRY(ctx->match_entry));
        const gchar *type_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ctx->type_combo));
        p.type = type_id ? type_id : "js";
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->code_view));
        GtkTextIter start, end;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_get_end_iter(buf, &end);
        gchar *code = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
        p.code = code ? code : "";
        g_free(code);
        p.enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ctx->enabled_check));

        if (!p.name.empty()) {
            if (ctx->index >= 0 && ctx->index < int(app->settings.extensions.size())) {
                app->settings.extensions[ctx->index] = p;
            } else {
                app->settings.extensions.push_back(p);
            }
            save_settings(app->settings);
            apply_extensions_to_all_tabs();
            refresh_extensions_ui();
        }
    }
    delete ctx;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void open_extension_dialog(int index) {
    const std::string &lang = app->settings.language;
    bool editing = index >= 0 && index < int(app->settings.extensions.size());

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        tr(editing ? Str::EditExtension : Str::AddExtension, lang), GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
        tr(Str::Cancel, lang), GTK_RESPONSE_CANCEL, tr(Str::Save, lang), GTK_RESPONSE_OK, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 480, 420);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    auto *ctx = new ExtensionDialogCtx();
    ctx->index = index;

    GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(name_row), gtk_label_new(tr(Str::NameField, lang)), FALSE, FALSE, 0);
    ctx->name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(ctx->name_entry, TRUE);
    if (editing) gtk_entry_set_text(GTK_ENTRY(ctx->name_entry), app->settings.extensions[index].name.c_str());
    gtk_box_pack_start(GTK_BOX(name_row), ctx->name_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), name_row, FALSE, FALSE, 0);

    GtkWidget *match_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(match_row), gtk_label_new(tr(Str::MatchPattern, lang)), FALSE, FALSE, 0);
    ctx->match_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ctx->match_entry), "https://example.com/*");
    gtk_widget_set_hexpand(ctx->match_entry, TRUE);
    if (editing) gtk_entry_set_text(GTK_ENTRY(ctx->match_entry), app->settings.extensions[index].match.c_str());
    gtk_box_pack_start(GTK_BOX(match_row), ctx->match_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), match_row, FALSE, FALSE, 0);

    GtkWidget *type_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(type_row), gtk_label_new(tr(Str::ExtensionType, lang)), FALSE, FALSE, 0);
    ctx->type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ctx->type_combo), "js", "JavaScript");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ctx->type_combo), "css", "CSS");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ctx->type_combo), editing ? app->settings.extensions[index].type.c_str() : "js");
    gtk_box_pack_start(GTK_BOX(type_row), ctx->type_combo, FALSE, FALSE, 0);
    ctx->enabled_check = gtk_check_button_new_with_label(tr(Str::Enabled, lang));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ctx->enabled_check), editing ? app->settings.extensions[index].enabled : true);
    gtk_box_pack_start(GTK_BOX(type_row), ctx->enabled_check, FALSE, FALSE, 12);
    gtk_box_pack_start(GTK_BOX(box), type_row, FALSE, FALSE, 0);

    GtkWidget *code_scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(code_scroller, TRUE);
    ctx->code_view = gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ctx->code_view), TRUE);
    if (editing) {
        GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx->code_view));
        gtk_text_buffer_set_text(buf, app->settings.extensions[index].code.c_str(), -1);
    }
    gtk_container_add(GTK_CONTAINER(code_scroller), ctx->code_view);
    gtk_box_pack_start(GTK_BOX(box), code_scroller, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(content), box);
    gtk_widget_show_all(dialog);
    g_signal_connect(dialog, "response", G_CALLBACK(on_extension_dialog_response), ctx);
}

static void on_extension_edit_clicked(GtkButton *, gpointer user_data) { open_extension_dialog(GPOINTER_TO_INT(user_data)); }
static void on_add_extension_clicked(GtkButton *, gpointer) { open_extension_dialog(-1); }

static void refresh_extensions_ui() {
    if (!app->extensions_listbox) return;
    clear_container(app->extensions_listbox);
    for (size_t i = 0; i < app->settings.extensions.size(); ++i) {
        const auto &p = app->settings.extensions[i];
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_container_set_border_width(GTK_CONTAINER(hbox), 6);

        GtkWidget *enabled_check = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled_check), p.enabled);
        g_signal_connect(enabled_check, "toggled", G_CALLBACK(on_extension_enabled_toggled), GINT_TO_POINTER(int(i)));

        GtkWidget *name_lbl = gtk_label_new(p.name.c_str());
        gtk_label_set_width_chars(GTK_LABEL(name_lbl), 18);
        gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);

        GtkWidget *type_lbl = gtk_label_new(p.type == "css" ? "CSS" : "JS");
        gtk_widget_set_size_request(type_lbl, 32, -1);

        GtkWidget *match_lbl = gtk_label_new(p.match.empty() ? "*" : p.match.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(match_lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(match_lbl), 0.0);
        gtk_widget_set_hexpand(match_lbl, TRUE);

        GtkWidget *edit_btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(edit_btn), GTK_RELIEF_NONE);
        gtk_button_set_image(GTK_BUTTON(edit_btn),
                              gtk_image_new_from_icon_name("document-edit-symbolic", GTK_ICON_SIZE_MENU));
        g_signal_connect(edit_btn, "clicked", G_CALLBACK(on_extension_edit_clicked), GINT_TO_POINTER(int(i)));

        GtkWidget *delete_btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(delete_btn), GTK_RELIEF_NONE);
        gtk_button_set_image(GTK_BUTTON(delete_btn),
                              gtk_image_new_from_icon_name("edit-delete-symbolic", GTK_ICON_SIZE_MENU));
        g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_extension_delete_clicked), GINT_TO_POINTER(int(i)));

        gtk_box_pack_start(GTK_BOX(hbox), enabled_check, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), name_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), type_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), match_lbl, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), edit_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), delete_btn, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_list_box_insert(GTK_LIST_BOX(app->extensions_listbox), row, -1);
    }
    gtk_widget_show_all(app->extensions_listbox);
}

static GtkWidget *build_extensions_page() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    const std::string &lang = app->settings.language;

    GtkWidget *header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add_btn = gtk_button_new_with_label(tr(Str::AddExtension, lang));
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_add_extension_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(header_row), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), header_row, FALSE, FALSE, 0);

    GtkWidget *scroller = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_vexpand(scroller, TRUE);
    app->extensions_listbox = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroller), app->extensions_listbox);
    gtk_box_pack_start(GTK_BOX(box), scroller, TRUE, TRUE, 0);
    return box;
}

static void open_extensions_tab() {
    if (app->extensions_tab) {
        switch_to_tab(app->extensions_tab);
        return;
    }
    Tab *tab = new Tab();
    tab->id = app->next_tab_id++;
    tab->internal_title = Str::Extensions;
    tab->page = build_extensions_page();
    gtk_widget_show_all(tab->page);

    char name[24];
    snprintf(name, sizeof(name), "tab%d", tab->id);
    gtk_stack_add_named(GTK_STACK(app->content_stack), tab->page, name);

    app->tabs.push_back(tab);
    app->extensions_tab = tab;
    run_chrome_js("photonAddTab(" + std::to_string(tab->id) + "," +
                  js_string_literal(tr(Str::Extensions, app->settings.language)) + ",null,\"\\u26a1\")");
    refresh_extensions_ui();
    switch_to_tab(tab);
}

static void on_extensions_clicked(GtkButton *, gpointer) { open_extensions_tab(); }

// -- appearance -----------------------------------------------------------

static void apply_theme() {
    GtkSettings *gs = gtk_settings_get_default();
    gboolean prefer_dark = app->settings.theme == "dark";
    g_object_set(gs, "gtk-application-prefer-dark-theme", prefer_dark, nullptr);
}

// Styles the native GTK chrome that remains (window background, settings cards).
// Explicitly colors every native GTK widget type used in Settings/Cookies/
// Bookmarks/Extensions, rather than only setting window/headerbar background
// and relying on gtk-application-prefer-dark-theme to get matching text
// colors from the system theme. That only worked by coincidence when the
// system theme happened to already be dark -- Light mode exposed it: a
// white window background with text colors still coming from the (dark)
// system theme, i.e. white-on-white. This makes the native chrome just as
// self-contained as the HTML chrome already is, independent of whatever
// theme the desktop happens to be running.
static void apply_appearance_css() {
    std::string bg, fg, card_bg, entry_bg, border;
    if (app->settings.theme == "dark") {
        bg = "#1a1b1e";
        fg = "#e8e8e8";
        card_bg = "#202124";
        entry_bg = "#26272b";
        border = "#38393d";
    } else if (app->settings.theme == "light") {
        bg = "#ffffff";
        fg = "#1a1b1e";
        card_bg = "#f2f2f2";
        entry_bg = "#ffffff";
        border = "#d0d0d0";
    } else {  // custom
        bg = app->settings.bg_color.empty() ? "#2b2c30" : app->settings.bg_color;
        fg = app->settings.fg_color.empty() ? "#ffffff" : app->settings.fg_color;
        card_bg = bg;
        entry_bg = bg;
        border = "#555555";
    }

    std::string css =
        "window, dialog { background-color: " + bg + "; color: " + fg + "; } "
        "headerbar { background-color: " + bg + "; background-image: none; border: none; box-shadow: none; "
        "color: " + fg + "; } "
        "label, checkbutton, radiobutton, button, combobox, spinbutton { color: " + fg + "; } "
        "entry, spinbutton entry, textview text { background-color: " + entry_bg + "; color: " + fg + "; } "
        "textview { background-color: " + entry_bg + "; } "
        ".settings-card { background-color: " + card_bg + "; border: 1px solid " + border + "; "
        "border-radius: 10px; padding: 14px; } "
        ".bookmarks-popup { background-color: " + card_bg + "; border: 1px solid " + border + "; "
        "border-radius: 8px; padding: 6px; } "
        ".bookmarks-popup button { background: none; border: none; padding: 8px 10px; } "
        ".bookmarks-popup button:hover { background-color: alpha(" + fg + ", 0.12); } ";

    if (!app->theme_css) {
        app->theme_css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(app->theme_css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    gtk_css_provider_load_from_data(app->theme_css, css.c_str(), -1, nullptr);
}

static void push_chrome_theme() {
    run_chrome_js("photonSetTheme(" + js_string_literal(app->settings.theme) + "," +
                  js_string_literal(app->settings.bg_color) + "," + js_string_literal(app->settings.fg_color) + ")");
}

static void push_chrome_tab_style() {
    run_chrome_js("photonSetTabStyle(" + std::to_string(app->settings.tab_radius) + "," +
                  std::to_string(app->settings.tab_height) + ")");
}

// The chrome is a real GTK widget with a fixed pixel height (it's not the page
// content, so it can't just grow/scroll) -- it must be kept in sync with the
// CSS row heights (tabs row + toolbar row), or bigger buttons/tabs get
// silently clipped at the bottom of the WebView's native allocation. The
// bookmarks popup floats separately via GtkOverlay (see App::bookmarks_popup)
// but still needs to be pinned just below the toolbar, so its top margin is
// kept in sync here too.
static void update_chrome_height() {
    if (!app->chrome_view) return;
    int height = app->settings.tab_height + app->settings.toolbar_size + 14;
    gtk_widget_set_size_request(GTK_WIDGET(app->chrome_view), -1, height);
    if (app->bookmarks_popup) gtk_widget_set_margin_top(app->bookmarks_popup, height);
}

static Str toolbar_item_label(const std::string &id) {
    if (id == "back") return Str::Back;
    if (id == "forward") return Str::Forward;
    if (id == "reload") return Str::Reload;
    if (id == "address") return Str::AddressBar;
    if (id == "settings") return Str::Settings;
    return Str::Bookmarks;
}

static void push_chrome_toolbar() {
    run_chrome_js("photonSetToolbarSize(" + std::to_string(app->settings.toolbar_size) + ")");
    std::string arr = "[";
    for (size_t i = 0; i < app->settings.toolbar_order.size(); ++i) {
        if (i) arr += ",";
        arr += js_string_literal(app->settings.toolbar_order[i]);
    }
    arr += "]";
    run_chrome_js("photonSetToolbarOrder(" + arr + ")");
}

// -- settings tab: card helper ---------------------------------------------

// Adds a titled "card" section to parent_box and returns its inner content box
// (already packed) for the caller to fill with rows.
static GtkWidget *begin_settings_card(GtkWidget *parent_box, const char *title) {
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "settings-card");
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(parent_box), frame, FALSE, FALSE, 0);

    GtkWidget *header = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(header), 0.0);
    set_bold(header, title);
    gtk_box_pack_start(GTK_BOX(inner), header, FALSE, FALSE, 0);
    return inner;
}

// -- settings tab: handlers -------------------------------------------------

static void on_clear_on_exit_toggled(GtkToggleButton *btn, gpointer) {
    app->settings.clear_on_exit = gtk_toggle_button_get_active(btn);
    save_settings(app->settings);
}

static void on_language_changed(GtkComboBox *combo, gpointer) {
    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (!id || app->settings.language == id) return;
    app->settings.language = id;
    save_settings(app->settings);
    refresh_language_ui();
}

static void on_search_engine_changed(GtkComboBox *combo, gpointer) {
    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    app->settings.search_engine = id;
    save_settings(app->settings);
}

static void on_theme_changed(GtkComboBox *combo, gpointer) {
    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (!id || app->settings.theme == id) return;
    app->settings.theme = id;
    save_settings(app->settings);
    apply_theme();
    apply_appearance_css();
    push_chrome_theme();
    request_settings_refresh();
}

static void on_bg_color_set(GtkColorButton *cb, gpointer) {
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cb), &rgba);
    gchar *s = gdk_rgba_to_string(&rgba);
    app->settings.bg_color = s;
    g_free(s);
    save_settings(app->settings);
    apply_appearance_css();
    push_chrome_theme();
}

static void on_bg_color_reset(GtkButton *, gpointer) {
    app->settings.bg_color.clear();
    save_settings(app->settings);
    apply_appearance_css();
    push_chrome_theme();
}

static void on_fg_color_set(GtkColorButton *cb, gpointer) {
    GdkRGBA rgba;
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(cb), &rgba);
    gchar *s = gdk_rgba_to_string(&rgba);
    app->settings.fg_color = s;
    g_free(s);
    save_settings(app->settings);
    push_chrome_theme();
}

static void on_fg_color_reset(GtkButton *, gpointer) {
    app->settings.fg_color.clear();
    save_settings(app->settings);
    push_chrome_theme();
}

static void on_tab_radius_changed(GtkSpinButton *spin, gpointer) {
    app->settings.tab_radius = gtk_spin_button_get_value_as_int(spin);
    save_settings(app->settings);
    push_chrome_tab_style();
}

static void on_toolbar_size_changed(GtkSpinButton *spin, gpointer) {
    app->settings.toolbar_size = gtk_spin_button_get_value_as_int(spin);
    save_settings(app->settings);
    push_chrome_toolbar();
    update_chrome_height();
}

static void on_tab_size_changed(GtkSpinButton *spin, gpointer) {
    app->settings.tab_height = gtk_spin_button_get_value_as_int(spin);
    save_settings(app->settings);
    push_chrome_tab_style();
    update_chrome_height();
}

static void move_toolbar_item(const std::string &id, int dir) {
    auto &v = app->settings.toolbar_order;
    auto it = std::find(v.begin(), v.end(), id);
    if (it == v.end()) return;
    size_t i = it - v.begin();
    int j = int(i) + dir;
    if (j < 0 || j >= int(v.size())) return;
    std::swap(v[i], v[j]);
    save_settings(app->settings);
    push_chrome_toolbar();
    request_settings_refresh();
}

static void on_toolbar_move_up(GtkButton *, gpointer user_data) {
    move_toolbar_item(static_cast<const char *>(user_data), -1);
}
static void on_toolbar_move_down(GtkButton *, gpointer user_data) {
    move_toolbar_item(static_cast<const char *>(user_data), +1);
}

// -- settings tab: assembly -------------------------------------------------
//
// The whole settings body is rebuilt from scratch on every change (language,
// theme, ...) instead of patching individual widgets in place. This is
// deliberately simple and avoids a whole class of dangling-pointer bugs from
// widgets outliving their tracked pointers.
//
// request_settings_refresh() must be used (instead of calling
// refresh_settings_content() directly) from any handler triggered by a widget
// that itself lives inside the settings content -- destroying that widget's
// own ancestor synchronously, from within its own signal emission, is a GTK
// reentrancy hazard that crashes. Deferring to an idle callback runs the
// rebuild safely after the triggering signal has fully unwound.

static gboolean idle_refresh_settings_content(gpointer) {
    refresh_settings_content();
    return G_SOURCE_REMOVE;
}

static void request_settings_refresh() { g_idle_add(idle_refresh_settings_content, nullptr); }

static void refresh_settings_content() {
    if (!app->settings_scroller) return;
    app->settings_status_label = nullptr;  // about to be destroyed; avoid a dangling target for async callbacks
    clear_container(app->settings_scroller);

    const std::string &lang = app->settings.language;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(box), 20);

    GtkWidget *title = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    set_bold(title, tr(Str::Settings, lang));
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

    // Privacy
    GtkWidget *privacy = begin_settings_card(box, tr(Str::Privacy, lang));
    GtkWidget *clear_check = gtk_check_button_new_with_label(tr(Str::ClearOnExit, lang));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(clear_check), app->settings.clear_on_exit);
    g_signal_connect(clear_check, "toggled", G_CALLBACK(on_clear_on_exit_toggled), nullptr);
    gtk_box_pack_start(GTK_BOX(privacy), clear_check, FALSE, FALSE, 0);

    GtkWidget *clear_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *clear_btn = gtk_button_new_with_label(tr(Str::ClearNow, lang));
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_now_clicked), nullptr);
    app->settings_status_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(clear_row), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(clear_row), app->settings_status_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(privacy), clear_row, FALSE, FALSE, 0);

    GtkWidget *adblock_check = gtk_check_button_new_with_label(tr(Str::BlockAds, lang));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(adblock_check), app->settings.adblock_enabled);
    g_signal_connect(adblock_check, "toggled", G_CALLBACK(on_adblock_toggled), nullptr);
    gtk_box_pack_start(GTK_BOX(privacy), adblock_check, FALSE, FALSE, 0);

    GtkWidget *ram_check = gtk_check_button_new_with_label(tr(Str::RamSavingMode, lang));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ram_check), app->settings.ram_saving_mode);
    g_signal_connect(ram_check, "toggled", G_CALLBACK(on_ram_saving_toggled), nullptr);
    gtk_box_pack_start(GTK_BOX(privacy), ram_check, FALSE, FALSE, 0);

    // Cookies
    GtkWidget *cookies = begin_settings_card(box, tr(Str::Cookies, lang));
    GtkWidget *cookies_btn = gtk_button_new_with_label(tr(Str::OpenCookieEditor, lang));
    gtk_widget_set_halign(cookies_btn, GTK_ALIGN_START);
    g_signal_connect(cookies_btn, "clicked", G_CALLBACK(on_cookies_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(cookies), cookies_btn, FALSE, FALSE, 0);

    // Extensions
    GtkWidget *extensions = begin_settings_card(box, tr(Str::Extensions, lang));
    GtkWidget *extensions_btn = gtk_button_new_with_label(tr(Str::ManageExtensions, lang));
    gtk_widget_set_halign(extensions_btn, GTK_ALIGN_START);
    g_signal_connect(extensions_btn, "clicked", G_CALLBACK(on_extensions_clicked), nullptr);
    gtk_box_pack_start(GTK_BOX(extensions), extensions_btn, FALSE, FALSE, 0);

    // Search
    GtkWidget *search = begin_settings_card(box, tr(Str::Search, lang));
    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *search_row_label = gtk_label_new(tr(Str::SearchEngine, lang));
    GtkWidget *search_combo = gtk_combo_box_text_new();
    for (const auto &engine : SEARCH_ENGINES) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(search_combo), engine.id, engine.name);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(search_combo), app->settings.search_engine.c_str());
    g_signal_connect(search_combo, "changed", G_CALLBACK(on_search_engine_changed), nullptr);
    gtk_box_pack_start(GTK_BOX(search_row), search_row_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search_row), search_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(search), search_row, FALSE, FALSE, 0);

    // Appearance
    GtkWidget *appearance = begin_settings_card(box, tr(Str::Appearance, lang));
    GtkWidget *theme_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *theme_row_label = gtk_label_new(tr(Str::Theme, lang));
    GtkWidget *theme_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), "dark", tr(Str::ThemeDark, lang));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), "light", tr(Str::ThemeLight, lang));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(theme_combo), "custom", tr(Str::ThemeCustom, lang));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(theme_combo), app->settings.theme.c_str());
    g_signal_connect(theme_combo, "changed", G_CALLBACK(on_theme_changed), nullptr);
    gtk_box_pack_start(GTK_BOX(theme_row), theme_row_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(theme_row), theme_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(appearance), theme_row, FALSE, FALSE, 0);

    if (app->settings.theme == "custom") {
        GtkWidget *bg_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *bg_row_label = gtk_label_new(tr(Str::CustomColor, lang));
        GdkRGBA bg_rgba{0.17, 0.17, 0.19, 1};
        if (!app->settings.bg_color.empty()) gdk_rgba_parse(&bg_rgba, app->settings.bg_color.c_str());
        GtkWidget *bg_color_btn = gtk_color_button_new_with_rgba(&bg_rgba);
        g_signal_connect(bg_color_btn, "color-set", G_CALLBACK(on_bg_color_set), nullptr);
        GtkWidget *bg_reset_btn = gtk_button_new_with_label(tr(Str::Reset, lang));
        g_signal_connect(bg_reset_btn, "clicked", G_CALLBACK(on_bg_color_reset), nullptr);
        gtk_box_pack_start(GTK_BOX(bg_row), bg_row_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bg_row), bg_color_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bg_row), bg_reset_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(appearance), bg_row, FALSE, FALSE, 0);

        GtkWidget *fg_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *fg_row_label = gtk_label_new(tr(Str::TextColor, lang));
        GdkRGBA fg_rgba{1, 1, 1, 1};
        if (!app->settings.fg_color.empty()) gdk_rgba_parse(&fg_rgba, app->settings.fg_color.c_str());
        GtkWidget *fg_color_btn = gtk_color_button_new_with_rgba(&fg_rgba);
        g_signal_connect(fg_color_btn, "color-set", G_CALLBACK(on_fg_color_set), nullptr);
        GtkWidget *fg_reset_btn = gtk_button_new_with_label(tr(Str::Reset, lang));
        g_signal_connect(fg_reset_btn, "clicked", G_CALLBACK(on_fg_color_reset), nullptr);
        gtk_box_pack_start(GTK_BOX(fg_row), fg_row_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(fg_row), fg_color_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(fg_row), fg_reset_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(appearance), fg_row, FALSE, FALSE, 0);
    }

    GtkWidget *radius_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *radius_label = gtk_label_new(tr(Str::TabShape, lang));
    GtkWidget *radius_spin = gtk_spin_button_new_with_range(0, 20, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(radius_spin), app->settings.tab_radius);
    g_signal_connect(radius_spin, "value-changed", G_CALLBACK(on_tab_radius_changed), nullptr);
    gtk_box_pack_start(GTK_BOX(radius_row), radius_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(radius_row), radius_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(appearance), radius_row, FALSE, FALSE, 0);

    // Language
    GtkWidget *language = begin_settings_card(box, tr(Str::Language, lang));
    GtkWidget *lang_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang_combo), "en", "English");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(lang_combo), "ru", "Русский");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(lang_combo), lang.c_str());
    g_signal_connect(lang_combo, "changed", G_CALLBACK(on_language_changed), nullptr);
    gtk_widget_set_halign(lang_combo, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(language), lang_combo, FALSE, FALSE, 0);

    // Toolbar: button size + reorder (position)
    GtkWidget *toolbar_card = begin_settings_card(box, tr(Str::Toolbar, lang));

    GtkWidget *size_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *size_label = gtk_label_new(tr(Str::Size, lang));
    GtkWidget *size_spin = gtk_spin_button_new_with_range(20, 40, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(size_spin), app->settings.toolbar_size);
    g_signal_connect(size_spin, "value-changed", G_CALLBACK(on_toolbar_size_changed), nullptr);
    gtk_box_pack_start(GTK_BOX(size_row), size_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(size_row), size_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar_card), size_row, FALSE, FALSE, 0);

    GtkWidget *tab_size_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *tab_size_label = gtk_label_new(tr(Str::TabSize, lang));
    GtkWidget *tab_size_spin = gtk_spin_button_new_with_range(32, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tab_size_spin), app->settings.tab_height);
    g_signal_connect(tab_size_spin, "value-changed", G_CALLBACK(on_tab_size_changed), nullptr);
    gtk_box_pack_start(GTK_BOX(tab_size_row), tab_size_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab_size_row), tab_size_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar_card), tab_size_row, FALSE, FALSE, 0);

    for (const auto &id : app->settings.toolbar_order) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name_lbl = gtk_label_new(tr(toolbar_item_label(id), lang));
        gtk_label_set_xalign(GTK_LABEL(name_lbl), 0.0);
        gtk_widget_set_hexpand(name_lbl, TRUE);
        GtkWidget *up_btn = gtk_button_new_from_icon_name("go-up-symbolic", GTK_ICON_SIZE_MENU);
        connect_with_id(up_btn, "clicked", G_CALLBACK(on_toolbar_move_up), id);
        GtkWidget *down_btn = gtk_button_new_from_icon_name("go-down-symbolic", GTK_ICON_SIZE_MENU);
        connect_with_id(down_btn, "clicked", G_CALLBACK(on_toolbar_move_down), id);
        gtk_box_pack_start(GTK_BOX(row), name_lbl, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), up_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), down_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(toolbar_card), row, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(app->settings_scroller), box);
    gtk_widget_show_all(box);
}

static void on_cookies_clicked(GtkButton *, gpointer) { open_cookie_editor_tab(); }

static void open_settings_tab() {
    if (app->settings_tab) {
        switch_to_tab(app->settings_tab);
        return;
    }
    Tab *tab = new Tab();
    tab->id = app->next_tab_id++;
    tab->internal_title = Str::Settings;
    tab->page = gtk_scrolled_window_new(nullptr, nullptr);
    app->settings_scroller = tab->page;
    refresh_settings_content();
    gtk_widget_show_all(tab->page);

    char name[24];
    snprintf(name, sizeof(name), "tab%d", tab->id);
    gtk_stack_add_named(GTK_STACK(app->content_stack), tab->page, name);

    app->tabs.push_back(tab);
    app->settings_tab = tab;
    run_chrome_js("photonAddTab(" + std::to_string(tab->id) + "," +
                  js_string_literal(tr(Str::Settings, app->settings.language)) + ",null,\"\\u2699\")");
    switch_to_tab(tab);
}

static void refresh_language_ui() {
    const std::string &lang = app->settings.language;
    if (app->settings_tab) request_settings_refresh();
    if (app->cookies_tab) {
        run_chrome_js("photonUpdateTab(" + std::to_string(app->cookies_tab->id) + "," +
                      js_string_literal(tr(Str::Cookies, lang)) + ",null)");
    }
    if (app->bookmarks_tab) {
        run_chrome_js("photonUpdateTab(" + std::to_string(app->bookmarks_tab->id) + "," +
                      js_string_literal(tr(Str::Bookmarks, lang)) + ",null)");
    }
    if (app->extensions_tab) {
        run_chrome_js("photonUpdateTab(" + std::to_string(app->extensions_tab->id) + "," +
                      js_string_literal(tr(Str::Extensions, lang)) + ",null)");
        refresh_extensions_ui();
    }
    if (app->settings_tab) {
        run_chrome_js("photonUpdateTab(" + std::to_string(app->settings_tab->id) + "," +
                      js_string_literal(tr(Str::Settings, lang)) + ",null)");
    }
    if (app->bookmarks_manage_btn) gtk_button_set_label(GTK_BUTTON(app->bookmarks_manage_btn), tr(Str::ManageBookmarks, lang));
    if (app->bookmarks_toggle_btn && app->bookmarks_popup_open) open_bookmarks_popup();  // refreshes its label too
    if (Tab *cur = current_tab()) update_window_title(cur);
}

// -- chrome HTML/CSS/JS -----------------------------------------------------

static const char *CHROME_HTML = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
:root {
  --bg:#18191c; --bg2:#1a1b1e; --fg:#e8e8e8; --fg-dim:#a9a9ad;
  --tab-bg:#202124; --tab-active:#2b2c30; --accent:#3a7afe; --border:rgba(128,128,128,0.25);
  --tab-radius:6px; --tab-height:44px;
}
body.theme-light {
  --bg:#ffffff; --bg2:#ffffff; --fg:#1a1b1e; --fg-dim:#4a4a4a;
  --tab-bg:#f1f1f1; --tab-active:#e4e4e4; --accent:#1a73e8; --border:rgba(0,0,0,0.12);
}
* { box-sizing:border-box; }
html,body { margin:0; padding:0; height:100%; overflow:hidden; background:var(--bg2); color:var(--fg);
            font-family:-apple-system,"Segoe UI",sans-serif; font-size:13px; }
#tabs { display:flex; align-items:stretch; gap:4px; height:var(--tab-height); padding:5px 5px 0; background:var(--bg);
        overflow-x:auto; overflow-y:hidden; }
#tabs::-webkit-scrollbar { height:0; }
.tab { display:flex; align-items:center; gap:8px; padding:0 12px; flex:1 1 0; min-width:96px; max-width:260px;
       background:var(--tab-bg); color:var(--fg-dim); border-radius:var(--tab-radius) var(--tab-radius) 0 0;
       cursor:default; user-select:none; font-size:14px; }
.tab.active { background:var(--tab-active); color:var(--fg); }
.tab.dragging { opacity:0.5; }
.favicon { width:16px; height:16px; flex-shrink:0; background-size:contain; background-repeat:no-repeat;
           background-position:center; text-align:center; line-height:16px; font-size:12px; }
.title { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; flex:1; }
.close { opacity:0.55; padding:1px 5px; border-radius:3px; font-size:14px; flex-shrink:0; }
.close:hover { opacity:1; background:rgba(128,128,128,0.35); }
#newtab { display:flex; align-items:center; justify-content:center; width:38px; color:var(--fg-dim);
          cursor:default; font-size:21px; font-weight:600; flex-shrink:0; }
#newtab:hover { color:var(--fg); background:rgba(128,128,128,0.15); }
#dragregion { flex:1; align-self:stretch; -webkit-user-select:none; }
#wincontrols { display:flex; align-items:stretch; flex-shrink:0; -webkit-user-select:none; }
.winbtn { display:flex; align-items:center; justify-content:center; width:38px; color:var(--fg-dim);
          cursor:default; font-size:13px; }
.winbtn:hover { background:rgba(128,128,128,0.2); color:var(--fg); }
.winbtn-close:hover { background:#e81123; color:#fff; }
:root { --btn-size:28px; }
#toolbar { display:flex; align-items:center; gap:4px; height:calc(var(--btn-size) + 14px); padding:0 6px;
           background:var(--bg2); border-top:1px solid var(--border); }
#toolbar button { border:none; background:none; color:var(--fg); width:var(--btn-size); height:var(--btn-size);
                   border-radius:6px; display:flex; align-items:center; justify-content:center; cursor:default;
                   padding:0; font-size:calc(var(--btn-size) * 0.55); }
#toolbar button:hover { background:rgba(128,128,128,0.2); }
#address-wrap { position:relative; flex:1; display:flex; }
#address { flex:1; width:100%; height:var(--btn-size); border-radius:6px; border:1px solid var(--border);
           background:var(--tab-bg); color:var(--fg); padding:0 10px; font-size:13px; outline:none; }
#address:focus { border-color:var(--accent); }
#suggestions { display:none; position:absolute; top:calc(100% + 4px); left:0; right:0; background:var(--tab-active);
               border:1px solid var(--border); border-radius:8px; overflow:hidden; z-index:20;
               box-shadow:0 4px 16px rgba(0,0,0,0.35); }
#suggestions.open { display:block; }
.suggest-item { padding:8px 12px; font-size:13px; color:var(--fg); white-space:nowrap; overflow:hidden;
                text-overflow:ellipsis; cursor:default; }
.suggest-item:hover, .suggest-item.active { background:rgba(128,128,128,0.25); }
#bookmarks-wrap { position:relative; }
</style></head>
<body class="theme-dark">
<div id="tabs"></div>
<div id="toolbar">
  <button id="back" title="Back">&#8249;</button>
  <button id="forward" title="Forward">&#8250;</button>
  <button id="reload" title="Reload">&#8635;</button>
  <div id="address-wrap">
    <input id="address" type="text" spellcheck="false">
    <div id="suggestions"></div>
  </div>
  <button id="settings" title="Settings">&#9881;</button>
  <div id="bookmarks-wrap">
    <!-- The dropdown itself is a native GTK popup floated over the page via
         GtkOverlay (see App::bookmarks_popup) -- a WebView can't render
         content past its own fixed-height allocation, so an HTML dropdown
         here could never truly layer on top of the page content below it. -->
    <button id="bookmarks" title="Bookmarks">&#8943;</button>
  </div>
  <div id="newtab" title="New Tab">+</div>
  <div id="dragregion"></div>
  <div id="wincontrols">
    <div id="win-min" class="winbtn" title="Minimize">&#8212;</div>
    <div id="win-max" class="winbtn" title="Maximize">&#9633;</div>
    <div id="win-close" class="winbtn winbtn-close" title="Close">&#10005;</div>
  </div>
</div>
<script>
function send(msg) { window.webkit.messageHandlers.photon.postMessage(msg); }

document.getElementById('back').addEventListener('click', function(){ send('back'); });
document.getElementById('forward').addEventListener('click', function(){ send('forward'); });
document.getElementById('reload').addEventListener('click', function(){ send('reload'); });
document.getElementById('newtab').addEventListener('click', function(){ send('newTab'); });
document.getElementById('settings').addEventListener('click', function(){ send('openSettings'); });

document.getElementById('win-min').addEventListener('click', function(){ send('winMinimize'); });
document.getElementById('win-max').addEventListener('click', function(){ send('winMaximize'); });
document.getElementById('win-close').addEventListener('click', function(){ send('winClose'); });
var dragEl = document.getElementById('dragregion');
dragEl.addEventListener('mousedown', function(e) {
  if (e.button !== 0) return;
  send('winDrag:' + Math.round(e.screenX) + ',' + Math.round(e.screenY));
});
dragEl.addEventListener('dblclick', function(){ send('winMaximize'); });

var addressEl = document.getElementById('address');
var suggestBox = document.getElementById('suggestions');
var suggestTimer = null;
var suggestIndex = -1;
var currentSuggestions = [];

function closeSuggestions() {
  currentSuggestions = [];
  suggestIndex = -1;
  suggestBox.innerHTML = '';
  suggestBox.classList.remove('open');
}

function highlightSuggestion() {
  var items = suggestBox.querySelectorAll('.suggest-item');
  items.forEach(function(el, i) { el.classList.toggle('active', i === suggestIndex); });
}

function photonShowSuggestions(list) {
  currentSuggestions = list || [];
  suggestIndex = -1;
  suggestBox.innerHTML = '';
  if (!currentSuggestions.length) { suggestBox.classList.remove('open'); return; }
  currentSuggestions.forEach(function(s) {
    var item = document.createElement('div');
    item.className = 'suggest-item';
    item.textContent = s;
    item.addEventListener('mousedown', function(e) {
      e.preventDefault();  // keep address bar focus so blur doesn't close this before the click lands
      addressEl.value = s;
      closeSuggestions();
      send('navigate:' + encodeURIComponent(s));
    });
    suggestBox.appendChild(item);
  });
  suggestBox.classList.add('open');
}

addressEl.addEventListener('input', function() {
  clearTimeout(suggestTimer);
  var q = addressEl.value.trim();
  if (!q) { closeSuggestions(); return; }
  suggestTimer = setTimeout(function() { send('suggest:' + encodeURIComponent(q)); }, 150);
});

addressEl.addEventListener('keydown', function(e) {
  if (e.key === 'Enter') {
    var value = (suggestIndex >= 0 && currentSuggestions[suggestIndex]) ? currentSuggestions[suggestIndex] : addressEl.value;
    closeSuggestions();
    send('navigate:' + encodeURIComponent(value));
  } else if (e.key === 'Escape') {
    closeSuggestions();
  } else if (e.key === 'ArrowDown' && currentSuggestions.length) {
    e.preventDefault();
    suggestIndex = (suggestIndex + 1) % currentSuggestions.length;
    highlightSuggestion();
  } else if (e.key === 'ArrowUp' && currentSuggestions.length) {
    e.preventDefault();
    suggestIndex = (suggestIndex - 1 + currentSuggestions.length) % currentSuggestions.length;
    highlightSuggestion();
  }
});
addressEl.addEventListener('blur', function() { setTimeout(closeSuggestions, 100); });

// The actual dropdown is a native GTK popup (see App::bookmarks_popup) --
// this button just tells native to show/hide it.
document.getElementById('bookmarks').addEventListener('click', function(e) {
  e.stopPropagation();
  send('bmMenuToggle');
});
document.addEventListener('click', function() { send('bmMenuClose'); });

// Tab order only ever matters visually -- native code always looks tabs up
// by id, never by position -- so reordering is handled entirely here with
// no round-trip to native at all.
var draggedTab = null;
var tabDragMoved = false;
function makeTabDraggable(el) {
  el.draggable = true;
  el.addEventListener('dragstart', function(e) {
    draggedTab = el;
    tabDragMoved = false;
    e.dataTransfer.effectAllowed = 'move';
    e.dataTransfer.setData('text/plain', el.dataset.id);
    el.classList.add('dragging');
  });
  el.addEventListener('dragend', function() {
    el.classList.remove('dragging');
    draggedTab = null;
  });
  el.addEventListener('dragover', function(e) {
    e.preventDefault();
    if (!draggedTab || draggedTab === el) return;
    tabDragMoved = true;
    var rect = el.getBoundingClientRect();
    var before = (e.clientX - rect.left) < rect.width / 2;
    el.parentNode.insertBefore(draggedTab, before ? el : el.nextSibling);
  });
  el.addEventListener('click', function() {
    if (tabDragMoved) { tabDragMoved = false; return; }
    send('switchTab:' + el.dataset.id);
  });
}

function photonAddTab(id, title, icon, glyph) {
  var el = document.createElement('div');
  el.className = 'tab';
  el.dataset.id = id;
  var fav = document.createElement('span');
  fav.className = 'favicon';
  if (icon) { fav.style.backgroundImage = 'url(' + icon + ')'; }
  else { fav.textContent = glyph || '\uD83C\uDF10'; }
  var titleEl = document.createElement('span');
  titleEl.className = 'title';
  titleEl.textContent = title;
  var closeEl = document.createElement('span');
  closeEl.className = 'close';
  closeEl.textContent = '\u00d7';
  closeEl.addEventListener('click', function(e){ e.stopPropagation(); send('closeTab:' + id); });
  el.appendChild(fav); el.appendChild(titleEl); el.appendChild(closeEl);
  makeTabDraggable(el);
  document.getElementById('tabs').insertBefore(el, document.getElementById('newtab'));
}
function photonUpdateTab(id, title, icon) {
  var el = document.querySelector('.tab[data-id="' + id + '"]');
  if (!el) return;
  if (title !== null) el.querySelector('.title').textContent = title;
  if (icon !== null) {
    var fav = el.querySelector('.favicon');
    fav.style.backgroundImage = 'url(' + icon + ')';
    fav.textContent = '';
  }
}
function photonRemoveTab(id) {
  var el = document.querySelector('.tab[data-id="' + id + '"]');
  if (el) el.remove();
}
function photonSetActiveTab(id) {
  document.querySelectorAll('.tab').forEach(function(el) {
    el.classList.toggle('active', el.dataset.id == id);
  });
}
function photonSetAddress(url) {
  if (document.activeElement !== addressEl) addressEl.value = url;
}
function photonSetLoading(loading) {
  document.getElementById('reload').innerHTML = loading ? '&#10005;' : '&#8635;';
}
function photonActivateTab(id, address) {
  photonSetActiveTab(id);
  photonSetAddress(address);
}
var CUSTOM_VARS = ['--bg', '--bg2', '--tab-bg', '--tab-active', '--fg', '--fg-dim'];
function photonSetTheme(theme, bgColor, fgColor) {
  document.body.className = 'theme-' + theme;
  CUSTOM_VARS.forEach(function(v) { document.body.style.removeProperty(v); });
  if (theme === 'custom') {
    if (bgColor) {
      document.body.style.setProperty('--bg', bgColor);
      document.body.style.setProperty('--bg2', bgColor);
      document.body.style.setProperty('--tab-bg', bgColor);
      document.body.style.setProperty('--tab-active', bgColor);
    }
    if (fgColor) {
      document.body.style.setProperty('--fg', fgColor);
      document.body.style.setProperty('--fg-dim', fgColor);
    }
  }
}
function photonSetTabStyle(radiusPx, heightPx) {
  document.documentElement.style.setProperty('--tab-radius', radiusPx + 'px');
  document.documentElement.style.setProperty('--tab-height', heightPx + 'px');
}
var TOOLBAR_DOM = { back:'back', forward:'forward', reload:'reload', address:'address-wrap',
                     settings:'settings', bookmarks:'bookmarks-wrap' };
function photonSetToolbarSize(px) {
  document.documentElement.style.setProperty('--btn-size', px + 'px');
}
function photonSetToolbarOrder(order) {
  var toolbar = document.getElementById('toolbar');
  order.forEach(function(key) {
    var el = document.getElementById(TOOLBAR_DOM[key] || key);
    if (el) toolbar.appendChild(el);
  });
}

var tabsEl = document.getElementById('tabs');
tabsEl.appendChild(document.getElementById('newtab'));
tabsEl.appendChild(document.getElementById('dragregion'));
tabsEl.appendChild(document.getElementById('wincontrols'));
</script>
</body></html>
)HTML";

// -- entry point ------------------------------------------------------

static void on_chrome_ready(WebKitWebView *view, WebKitLoadEvent event, gpointer) {
    if (event != WEBKIT_LOAD_FINISHED) return;
    g_signal_handlers_disconnect_by_func(view, (gpointer)on_chrome_ready, nullptr);

    push_chrome_theme();
    push_chrome_toolbar();
    push_chrome_tab_style();

    // The very first tab must be created here, not in main(): main() calls
    // this before gtk_main() has pumped a single event, so the chrome's own
    // load-html hasn't actually happened yet at that point and none of its
    // JS functions exist -- an earlier new_tab() call would silently fail
    // to add a pill (the tab's content still loads fine natively, but it
    // looks like the browser opened with zero tabs).
    new_tab(app->initial_uri, true, nullptr);
}

int main(int argc, char **argv) {
    // KWin's native-Wayland path doesn't reliably honor
    // gtk_window_set_decorated(FALSE) -- GTK3 only asks for *client-side*
    // decorations (i.e. none, since we don't draw a headerbar either) via
    // the xdg-decoration protocol when it's decided one way or the other,
    // and on Wayland KWin has historically defaulted undecided/legacy GTK
    // windows to its own server-side titlebar instead. XWayland's X11 path
    // doesn't have that ambiguity: window managers there decorate purely
    // off Motif hints, which gtk_window_set_decorated(FALSE) sets directly
    // and KWin has always respected. So under Wayland, route through
    // XWayland just for this app, unless the user already forced a backend.
    if (!g_getenv("GDK_BACKEND")) {
        const char *session = g_getenv("XDG_SESSION_TYPE");
        if (session && g_strcmp0(session, "wayland") == 0) g_setenv("GDK_BACKEND", "x11", TRUE);
    }
    gtk_init(&argc, &argv);

    app = new App();
    app->settings = load_settings();
    app->http_session = soup_session_new();

    std::string data_dir = std::string(g_get_home_dir()) + "/.local/share/photon-browser";
    std::string cache_dir = data_dir + "/cache";
    std::string data_subdir = data_dir + "/data";
    std::string cookie_path = data_dir + "/cookies.sqlite";
    std::string favicon_dir = data_dir + "/favicons";

    app->data_manager = webkit_website_data_manager_new("base-cache-directory", cache_dir.c_str(),
                                                          "base-data-directory", data_subdir.c_str(), nullptr);
    WebKitCookieManager *cookie_manager = webkit_website_data_manager_get_cookie_manager(app->data_manager);
    webkit_cookie_manager_set_persistent_storage(cookie_manager, cookie_path.c_str(),
                                                  WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    app->web_context = webkit_web_context_new_with_website_data_manager(app->data_manager);
    webkit_web_context_set_favicon_database_directory(app->web_context, favicon_dir.c_str());
    // DOCUMENT_BROWSER caches a moderate number of resources instead of the
    // default WEB_BROWSER model's "cache a very large number of resources
    // and previously viewed content" -- meaningfully lower memory at the
    // cost of a bit more re-fetching on repeat visits.
    webkit_web_context_set_cache_model(app->web_context, WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
    compile_adblock_filter();

    apply_theme();

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 750);
    gtk_window_set_title(GTK_WINDOW(app->window), "Photon Browser");
    // No native titlebar at all -- window controls (minimize/maximize/close) and
    // dragging live in the HTML chrome itself, so tabs+toolbar get the full
    // top of the window instead of sharing it with an empty GtkHeaderBar.
    gtk_window_set_decorated(GTK_WINDOW(app->window), FALSE);
    {
        GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
        if (gdk_pixbuf_loader_write(loader, PHOTON_ICON_PNG, PHOTON_ICON_PNG_LEN, nullptr) &&
            gdk_pixbuf_loader_close(loader, nullptr)) {
            gtk_window_set_icon(GTK_WINDOW(app->window), gdk_pixbuf_loader_get_pixbuf(loader));
        }
        g_object_unref(loader);
    }
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(app->window, "delete-event", G_CALLBACK(on_delete_event), nullptr);

    apply_appearance_css();

    GtkWidget *chrome_widget = webkit_web_view_new_with_context(app->web_context);
    app->chrome_view = WEBKIT_WEB_VIEW(chrome_widget);
    apply_lightweight_settings(app->chrome_view);
    update_chrome_height();

    WebKitUserContentManager *ucm = webkit_web_view_get_user_content_manager(app->chrome_view);
    g_signal_connect(ucm, "script-message-received::photon", G_CALLBACK(on_chrome_message), nullptr);
    webkit_user_content_manager_register_script_message_handler(ucm, "photon");
    g_signal_connect(app->chrome_view, "load-changed", G_CALLBACK(on_chrome_ready), nullptr);
    webkit_web_view_load_html(app->chrome_view, CHROME_HTML, nullptr);

    app->content_stack = gtk_stack_new();
    gtk_widget_set_vexpand(app->content_stack, TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), chrome_widget, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), app->content_stack, TRUE, TRUE, 0);

    // GtkOverlay so the bookmarks popup floats on top of the page content
    // instead of having to grow the chrome WebView and push everything down
    // -- see App::bookmarks_popup for why it has to be a native widget here.
    GtkWidget *overlay = gtk_overlay_new();
    gtk_container_add(GTK_CONTAINER(overlay), vbox);
    app->bookmarks_popup = build_bookmarks_popup();
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), app->bookmarks_popup);
    gtk_widget_set_halign(app->bookmarks_popup, GTK_ALIGN_END);
    gtk_widget_set_valign(app->bookmarks_popup, GTK_ALIGN_START);
    gtk_container_add(GTK_CONTAINER(app->window), overlay);
    update_chrome_height();  // now that the popup exists, sync its top margin too

    app->initial_uri = (argc > 1) ? to_uri(argv[1]) : home_uri();

    gtk_widget_show_all(app->window);
    gtk_main();

    save_settings(app->settings);
    return 0;
}
