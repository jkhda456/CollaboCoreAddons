/*
 * wpe-browser: a web browser for agents in the collaboCore guest — WPE WebKit showing one page on a
 * canvas of the guest's display (`gui`), and a control socket the `web` command talks to.
 *
 *   wpe-browser [--name NAME] [--size WxH] [--ephemeral] [--data-dir DIR] [--ignore-tls-errors]
 *               [--user-agent UA] [URL]
 *
 * The socket is $COLLABO_WEB_DIR/NAME.sock (/tmp/.collabo-web by default). A request is a list of
 * words: "N\n" then N times "LENGTH\n" and the bytes; the answer is "STATUS LENGTH\n" and the
 * bytes (status 0 done, 1 failed, 2 usage, 3 timed out, 4 not found). See web.c for the commands.
 *
 * Input an agent asks for (click, type, key, scroll) is sent into the page as the display's own
 * pointer and keyboard events would be, so pages see real events (hover, focus, keydown...).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

#include "browser-js.h" /* snapshot_js, target_js: the scripts, as C strings (gen-js.py) */

enum { ST_OK = 0, ST_FAILED = 1, ST_USAGE = 2, ST_TIMEOUT = 3, ST_NOT_FOUND = 4 };

typedef struct Browser Browser;
typedef struct Request Request;
typedef void (*JSDone)(Request *r, JSCValue *value, GError *error);

struct Browser {
    char *name;
    char *socket_path;
    WebKitWebView *view;
    WebKitNetworkSession *session;
    GMainLoop *loop;
    GSocketService *service;
    GList *load_waiters;        /* Requests waiting for the current navigation to end */
    gboolean loading;
    unsigned load_generation;   /* counts started navigations */
    char *last_load_error;
    char *last_dialog;
    unsigned web_process_crashes;
    unsigned modifiers;         /* WPEModifiers held by injected key events */
    unsigned sent[3];           /* input sent to the page: mousedown, mouseup, keydown (input sync) */
    gboolean serving;           /* answering requests: from the end of the first load */
    WPEView *wpe_view;          /* for the ready line */
};

struct Request {
    Browser *browser;
    GSocketConnection *connection;
    GByteArray *input;
    char **argv;
    int argc;
    guint timeout_id;
    guint poll_id;
    unsigned load_generation;   /* navigation started at or after this one ends the wait */
    gboolean wants_load;
    char *done_text;            /* the answer when the load ends */
    char *poll_kind, *poll_arg; /* wait text / selector */
    gint64 deadline;
    /* input sync: what the page had seen and what had been sent before this request's input */
    void (*send)(Request *r);
    void (*synced)(Request *r);
    gboolean have_base, generation_set;
    unsigned seen_base[3], sent_base[3];
    gint64 sync_deadline;
    guint sync_id;
    double x, y;                /* where the request acts, and what is there */
    char *what;
};

static Browser browser;

/* WPE_BROWSER_DEBUG=1: what happens to loads and waits, on stderr (`gui logs NAME`) */
static gboolean debugging;
#define DEBUG(...) do { if (debugging) g_printerr("wpe-browser: " __VA_ARGS__); } while (0)

/* ── answering ─────────────────────────────────────────────────────────────────────────────── */

static void request_free(Request *r)
{
    if (r->timeout_id)
        g_source_remove(r->timeout_id);
    if (r->poll_id)
        g_source_remove(r->poll_id);
    if (r->sync_id)
        g_source_remove(r->sync_id);
    browser.load_waiters = g_list_remove(browser.load_waiters, r);
    if (r->connection) {
        g_io_stream_close(G_IO_STREAM(r->connection), NULL, NULL);
        g_object_unref(r->connection);
    }
    if (r->input)
        g_byte_array_unref(r->input);
    g_strfreev(r->argv);
    g_free(r->done_text);
    g_free(r->poll_kind);
    g_free(r->poll_arg);
    g_free(r->what);
    g_free(r);
}

static void reply(Request *r, int status, const char *text)
{
    if (!text)
        text = "";
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(r->connection));
    char *head = g_strdup_printf("%d %zu\n", status, strlen(text));
    g_output_stream_write_all(out, head, strlen(head), NULL, NULL, NULL);
    g_output_stream_write_all(out, text, strlen(text), NULL, NULL, NULL);
    g_output_stream_flush(out, NULL, NULL);
    g_free(head);
    request_free(r);
}

static void replyf(Request *r, int status, const char *format, ...) G_GNUC_PRINTF(3, 4);
static void replyf(Request *r, int status, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *text = g_strdup_vprintf(format, args);
    va_end(args);
    reply(r, status, text);
    g_free(text);
}

static char *page_state(void)
{
    const char *uri = webkit_web_view_get_uri(browser.view);
    const char *title = webkit_web_view_get_title(browser.view);
    return g_strdup_printf("%s\n%s\n", uri ? uri : "about:blank", title ? title : "");
}

/* ── navigation and waiting for it ────────────────────────────────────────────────────────── */

static void call_js(Request *r, const char *body, GVariant *args, JSDone done);

/* The answer to a finished load: its address and the document's title (the view's title
 * property can come after the load has finished). */
static void loaded_title_read(Request *r, JSCValue *value, GError *error)
{
    char *title = !error && value && jsc_value_is_string(value) ? jsc_value_to_string(value) : NULL;
    const char *uri = webkit_web_view_get_uri(browser.view);
    const char *view_title = webkit_web_view_get_title(browser.view);
    replyf(r, ST_OK, "%s%s\n%s\n", r->done_text ? r->done_text : "", uri ? uri : "about:blank",
           title ? title : view_title ? view_title : "");
    g_free(title);
}

static void reply_loaded(Request *r)
{
    browser.load_waiters = g_list_remove(browser.load_waiters, r);
    r->wants_load = FALSE;
    if (r->timeout_id) {
        g_source_remove(r->timeout_id);
        r->timeout_id = 0;
    }
    if (r->poll_id) {
        g_source_remove(r->poll_id);
        r->poll_id = 0;
    }
    call_js(r, "return document.title;", NULL, loaded_title_read);
}

static void finish_load_waiters(int status, const char *why)
{
    GList *waiters = g_list_copy(browser.load_waiters);
    for (GList *l = waiters; l; l = l->next) {
        Request *r = l->data;
        /* a load that started before the request (the first about:blank, a load the new one
         * replaces) is not the one it waits for */
        if (!r->wants_load || browser.load_generation <= r->load_generation)
            continue;
        DEBUG("%s: answered (%s)\n", r->argv[0], why ? why : "loaded");
        if (status == ST_OK) {
            reply_loaded(r);
            continue;
        }
        char *state = page_state();
        replyf(r, status, "%s\n%s", why, state);
        g_free(state);
    }
    g_list_free(waiters);
}

static gboolean load_timed_out(gpointer data)
{
    Request *r = data;
    r->timeout_id = 0;
    char *state = page_state();
    replyf(r, ST_TIMEOUT, "still loading (%.0f%%); the page may be usable already\n%s",
           webkit_web_view_get_estimated_load_progress(browser.view) * 100, state);
    g_free(state);
    return G_SOURCE_REMOVE;
}

/* Answer `r` when the next navigation ends (or now, if none starts within `start_grace_ms`). */
static gboolean grace_over(gpointer data)
{
    Request *r = data;
    r->poll_id = 0;
    DEBUG("%s: no load started within the grace (loading %d, generation %u/%u)\n", r->argv[0], browser.loading,
          browser.load_generation, r->load_generation);
    if (browser.loading || browser.load_generation > r->load_generation)
        return G_SOURCE_REMOVE; /* a load started: the waiter answers when it ends */
    char *state = page_state();
    replyf(r, ST_OK, "%s%s", r->done_text ? r->done_text : "", state);
    g_free(state);
    return G_SOURCE_REMOVE;
}

static void reply_loaded(Request *r);

static void wait_for_load(Request *r, int timeout_ms, int start_grace_ms)
{
    r->wants_load = TRUE;
    /* input commands count from when they sent their input (input_base_read): the load it
     * started may be over already */
    if (!r->generation_set)
        r->load_generation = browser.load_generation;
    if (browser.load_generation > r->load_generation && !browser.loading) {
        if (browser.last_load_error) {
            char *state = page_state();
            replyf(r, ST_FAILED, "%s\n%s", browser.last_load_error, state);
            g_free(state);
        } else
            reply_loaded(r);
        return;
    }
    browser.load_waiters = g_list_append(browser.load_waiters, r);
    r->timeout_id = g_timeout_add(timeout_ms, load_timed_out, r);
    if (start_grace_ms >= 0 && !browser.loading)
        r->poll_id = g_timeout_add(start_grace_ms, grace_over, r);
}

/* Requests are answered from the end of the browser's first load (about:blank or the page it
 * was started with): its events would otherwise end the waits of the first requests. Connections
 * made before wait in the socket's backlog. */
static void start_serving(void)
{
    if (browser.serving)
        return;
    browser.serving = TRUE;
    g_socket_service_start(browser.service);
    g_print("wpe-browser: \"%s\" ready on %s (display: %s)\n", browser.name, browser.socket_path,
            browser.wpe_view ? G_OBJECT_TYPE_NAME(wpe_view_get_display(browser.wpe_view)) : "none");
}

static gboolean start_serving_anyway(gpointer data)
{
    (void)data;
    start_serving();
    return G_SOURCE_REMOVE;
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer data)
{
    (void)data;
    DEBUG("load %s: %s (generation %u, %u waiting)\n",
          event == WEBKIT_LOAD_STARTED ? "started" : event == WEBKIT_LOAD_REDIRECTED ? "redirected"
          : event == WEBKIT_LOAD_COMMITTED ? "committed" : "finished",
          webkit_web_view_get_uri(view), browser.load_generation, g_list_length(browser.load_waiters));
    switch (event) {
    case WEBKIT_LOAD_STARTED:
        browser.loading = TRUE;
        browser.load_generation++;
        g_clear_pointer(&browser.last_load_error, g_free);
        break;
    case WEBKIT_LOAD_FINISHED:
        browser.loading = FALSE;
        finish_load_waiters(browser.last_load_error ? ST_FAILED : ST_OK, browser.last_load_error);
        start_serving();
        break;
    default:
        break;
    }
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event, const char *uri, GError *error, gpointer data)
{
    (void)view; (void)event; (void)data;
    DEBUG("load failed: %s: %s (%s %d)\n", uri, error->message, g_quark_to_string(error->domain), error->code);
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        return FALSE;
    g_free(browser.last_load_error);
    if (error->domain == G_TLS_ERROR) {
        /* certificates and the status responses servers staple to them are checked against the
         * clock: a sandbox whose clock is days off fails here on sites that are fine */
        GDateTime *now = g_date_time_new_now_utc();
        char *when = g_date_time_format(now, "%Y-%m-%d %H:%M UTC");
        browser.last_load_error = g_strdup_printf("failed to load %s: %s (checked against this sandbox's clock, %s)",
                                                  uri, error->message, when);
        g_free(when);
        g_date_time_unref(now);
    } else
        browser.last_load_error = g_strdup_printf("failed to load %s: %s", uri, error->message);
    g_printerr("wpe-browser: %s\n", browser.last_load_error);
    /* handled: WebKit's error page would be a load of its own, which would end the waits as a
     * success; the page stays as it was and the error goes to whoever waits (and `web info`) */
    return TRUE;
}

static gboolean on_tls_errors(WebKitWebView *view, const char *uri, GTlsCertificate *certificate, GTlsCertificateFlags errors, gpointer data)
{
    (void)view; (void)certificate; (void)data;
    g_free(browser.last_load_error);
    browser.last_load_error = g_strdup_printf("failed to load %s: the server's certificate is not trusted (TLS errors 0x%x; "
                                              "wpe-browser --ignore-tls-errors to load it anyway)", uri, errors);
    return FALSE;
}

static char *normalize_uri(const char *text)
{
    if (strstr(text, "://") || g_str_has_prefix(text, "about:") || g_str_has_prefix(text, "data:") || g_str_has_prefix(text, "file:"))
        return g_strdup(text);
    if (text[0] == '/')
        return g_strconcat("file://", text, NULL);
    if (!strchr(text, ' ') && (strchr(text, '.') || g_str_has_prefix(text, "localhost")))
        return g_strconcat(strncmp(text, "localhost", 9) ? "https://" : "http://", text, NULL);
    /* words: a search */
    char *query = g_uri_escape_string(text, NULL, FALSE);
    char *uri = g_strconcat("https://html.duckduckgo.com/html/?q=", query, NULL);
    g_free(query);
    return uri;
}

/* ── JavaScript ────────────────────────────────────────────────────────────────────────────── */


typedef struct {
    Request *r;
    JSDone done;
} JSCall;

static void js_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    JSCall *call = data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_call_async_javascript_function_finish(WEBKIT_WEB_VIEW(source), result, &error);
    call->done(call->r, value, error);
    if (value)
        g_object_unref(value);
    g_clear_error(&error);
    g_free(call);
}

static void eval_finished(GObject *source, GAsyncResult *result, gpointer data)
{
    JSCall *call = data;
    GError *error = NULL;
    JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
    call->done(call->r, value, error);
    if (value)
        g_object_unref(value);
    g_clear_error(&error);
    g_free(call);
}

/* Runs `body` (an async function's body) with `args` (a{sv}, floating ok) as its variables. */
static void call_js(Request *r, const char *body, GVariant *args, JSDone done)
{
    JSCall *call = g_new0(JSCall, 1);
    call->r = r;
    call->done = done;
    webkit_web_view_call_async_javascript_function(browser.view, body, -1, args, NULL, NULL, NULL, js_finished, call);
}

static char *js_value_text(JSCValue *value)
{
    if (!value || jsc_value_is_undefined(value))
        return g_strdup("undefined");
    if (jsc_value_is_string(value))
        return jsc_value_to_string(value);
    char *json = jsc_value_to_json(value, 2);
    return json ? json : jsc_value_to_string(value);
}

static void reply_js_text(Request *r, JSCValue *value, GError *error)
{
    if (error) {
        replyf(r, ST_FAILED, "JavaScript error: %s\n", error->message);
        return;
    }
    char *text = js_value_text(value);
    size_t length = strlen(text);
    replyf(r, ST_OK, "%s%s", text, length && text[length - 1] == '\n' ? "" : "\n");
    g_free(text);
}

static GVariant *args_dict(const char *first_key, ...)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    va_list ap;
    va_start(ap, first_key);
    for (const char *key = first_key; key; key = va_arg(ap, const char *))
        g_variant_builder_add(&builder, "{sv}", key, va_arg(ap, GVariant *));
    va_end(ap);
    return g_variant_builder_end(&builder);
}

/* ── input, as the display would send it ──────────────────────────────────────────────────── */

static WPEView *wpe_view(void)
{
    return webkit_web_view_get_wpe_view(browser.view);
}

static guint32 now_ms(void)
{
    return (guint32)(g_get_monotonic_time() / 1000);
}

static void send_event(WPEEvent *event)
{
    wpe_view_event(wpe_view(), event);
    wpe_event_unref(event);
}

static void pointer_move(double x, double y)
{
    send_event(wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, wpe_view(), WPE_INPUT_SOURCE_MOUSE, now_ms(),
                                          (WPEModifiers)browser.modifiers, x, y, 0, 0));
}

static void pointer_click(double x, double y, guint button, int count)
{
    pointer_move(x, y);
    unsigned held = button == 1 ? WPE_MODIFIER_POINTER_BUTTON1 : button == 2 ? WPE_MODIFIER_POINTER_BUTTON2 : WPE_MODIFIER_POINTER_BUTTON3;
    for (int i = 1; i <= count; i++) {
        send_event(wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, wpe_view(), WPE_INPUT_SOURCE_MOUSE, now_ms(),
                                                (WPEModifiers)browser.modifiers, button, x, y, i));
        send_event(wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, wpe_view(), WPE_INPUT_SOURCE_MOUSE, now_ms(),
                                                (WPEModifiers)(browser.modifiers | held), button, x, y, 0));
        browser.sent[0]++;
        browser.sent[1]++;
    }
}

typedef struct { const char *name; guint keysym; guint code; } KeyName;
static const KeyName key_names[] = {
    { "Enter", 0xff0d, 28 }, { "Return", 0xff0d, 28 }, { "Tab", 0xff09, 15 }, { "Escape", 0xff1b, 1 },
    { "Esc", 0xff1b, 1 }, { "Backspace", 0xff08, 14 }, { "BackSpace", 0xff08, 14 }, { "Delete", 0xffff, 111 },
    { "Insert", 0xff63, 110 }, { "Home", 0xff50, 102 }, { "End", 0xff57, 107 }, { "PageUp", 0xff55, 104 },
    { "Page_Up", 0xff55, 104 }, { "PageDown", 0xff56, 109 }, { "Page_Down", 0xff56, 109 },
    { "ArrowLeft", 0xff51, 105 }, { "Left", 0xff51, 105 }, { "ArrowUp", 0xff52, 103 }, { "Up", 0xff52, 103 },
    { "ArrowRight", 0xff53, 106 }, { "Right", 0xff53, 106 }, { "ArrowDown", 0xff54, 108 }, { "Down", 0xff54, 108 },
    { "Space", ' ', 57 }, { "space", ' ', 57 }, { "F1", 0xffbe, 59 }, { "F2", 0xffbf, 60 }, { "F3", 0xffc0, 61 },
    { "F4", 0xffc1, 62 }, { "F5", 0xffc2, 63 }, { "F6", 0xffc3, 64 }, { "F7", 0xffc4, 65 }, { "F8", 0xffc5, 66 },
    { "F9", 0xffc6, 67 }, { "F10", 0xffc7, 68 }, { "F11", 0xffc8, 87 }, { "F12", 0xffc9, 88 },
};

/* The US keyboard's key for an ASCII character: its evdev code, and whether it needs Shift. */
static guint ascii_code(gunichar c, gboolean *shift)
{
    static const char row[] = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
    static const char shifted[] = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
    static const guint codes[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                                   30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53 };
    *shift = FALSE;
    if (c == ' ')
        return 57;
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++) {
        if ((gunichar)row[i] == c)
            return codes[i];
        if ((gunichar)shifted[i] == c) {
            *shift = TRUE;
            return codes[i];
        }
    }
    return 0;
}

static void key_event(gboolean down, guint keysym, guint code)
{
    if (down)
        browser.sent[2]++;
    send_event(wpe_event_keyboard_new(down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, wpe_view(),
                                      WPE_INPUT_SOURCE_KEYBOARD, now_ms(), (WPEModifiers)browser.modifiers,
                                      code ? code + 8 : 0, keysym));
}

static void modifier(gboolean down, unsigned bit, guint keysym, guint code)
{
    if (down) {
        key_event(TRUE, keysym, code);
        browser.modifiers |= bit;
    } else {
        browser.modifiers &= ~bit;
        key_event(FALSE, keysym, code);
    }
}

static void type_char(gunichar c)
{
    if (c == '\n') {
        key_event(TRUE, 0xff0d, 28);
        key_event(FALSE, 0xff0d, 28);
        return;
    }
    if (c == '\t') {
        key_event(TRUE, 0xff09, 15);
        key_event(FALSE, 0xff09, 15);
        return;
    }
    gboolean shift;
    guint code = c < 0x80 ? ascii_code(c, &shift) : 0;
    guint keysym = (c >= 0x20 && c < 0x7f) ? c : 0x01000000 + c; /* X11: Unicode keysyms */
    gboolean add_shift = code && shift && !(browser.modifiers & WPE_MODIFIER_KEYBOARD_SHIFT);
    if (add_shift)
        modifier(TRUE, WPE_MODIFIER_KEYBOARD_SHIFT, 0xffe1, 42);
    key_event(TRUE, keysym, code);
    key_event(FALSE, keysym, code);
    if (add_shift)
        modifier(FALSE, WPE_MODIFIER_KEYBOARD_SHIFT, 0xffe1, 42);
}

/* ── input sync ─────────────────────────────────────────────────────────────────────────────
 * Input reaches the page through queues of WebKit's UI process: key events one at a time (each
 * after the page took the one before), pointer events apart from them. A click can overtake keys
 * typed before it, and an answer can come before the page has seen the input. So a command that
 * sends input reads how much the page has seen (INPUT_COUNTER_JS counts it at the window, from
 * the start of each document), sends its own, and goes on once the page has seen that too —
 * or after INPUT_SYNC_MS (a frame of another origin, a page that went away). */
#define INPUT_SYNC_MS 3000
static const char INPUT_COUNTER_JS[] =
    "(() => { let w = window;"
    " try { if (window.top !== window && window.top.__wpeSeen) w = window.top; } catch (e) {}"
    " if (w === window && window.__wpeSeen) return;"
    " if (w === window) window.__wpeSeen = { mousedown: 0, mouseup: 0, keydown: 0 };"
    " for (const t of ['mousedown', 'mouseup', 'keydown']) window.addEventListener(t, () => { w.__wpeSeen[t]++; }, true); })();";
static const char INPUT_SEEN_JS[] =
    "const s = window.__wpeSeen; return s ? `${s.mousedown} ${s.mouseup} ${s.keydown}` : '';";

static gboolean parse_seen(JSCValue *value, unsigned seen[3])
{
    if (!value || !jsc_value_is_string(value))
        return FALSE;
    char *text = jsc_value_to_string(value);
    gboolean ok = sscanf(text, "%u %u %u", &seen[0], &seen[1], &seen[2]) == 3;
    g_free(text);
    return ok;
}

static void input_poll(Request *r);

static gboolean input_poll_again(gpointer data)
{
    Request *r = data;
    r->sync_id = 0;
    input_poll(r);
    return G_SOURCE_REMOVE;
}

static void input_polled(Request *r, JSCValue *value, GError *error)
{
    unsigned seen[3];
    gboolean done = error || !parse_seen(value, seen) || g_get_monotonic_time() > r->sync_deadline;
    for (int i = 0; !done && i < 3; i++) {
        if (seen[i] < r->seen_base[i]) /* another document: the input went to the one before */
            done = TRUE;
    }
    if (!done) {
        done = TRUE;
        for (int i = 0; i < 3; i++) {
            if (seen[i] - r->seen_base[i] < browser.sent[i] - r->sent_base[i])
                done = FALSE;
        }
    }
    if (done)
        r->synced(r);
    else
        r->sync_id = g_timeout_add(25, input_poll_again, r);
}

static void input_poll(Request *r)
{
    if (!r->have_base) {
        r->synced(r);
        return;
    }
    call_js(r, INPUT_SEEN_JS, NULL, input_polled);
}

static void input_base_read(Request *r, JSCValue *value, GError *error)
{
    r->have_base = !error && parse_seen(value, r->seen_base);
    if (!r->generation_set) {
        r->load_generation = browser.load_generation;
        r->generation_set = TRUE;
    }
    memcpy(r->sent_base, browser.sent, sizeof browser.sent);
    r->sync_deadline = g_get_monotonic_time() + INPUT_SYNC_MS * 1000;
    r->send(r);
    input_poll(r);
}

/* send(r) sends input; synced(r) runs when the page has seen it */
static void input_send(Request *r, void (*send)(Request *r), void (*synced)(Request *r))
{
    r->send = send;
    r->synced = synced;
    call_js(r, INPUT_SEEN_JS, NULL, input_base_read);
}

/* "ctrl+shift+Tab", "Enter", "a": FALSE if not understood; sent when `send` */
static gboolean key_spec(const char *spec, gboolean send)
{
    char **parts = g_strsplit(spec, "+", -1);
    int n = g_strv_length(parts);
    if (!n || !*parts[n - 1]) {
        g_strfreev(parts);
        return FALSE;
    }
    unsigned bits[8] = { 0 };
    guint syms[8] = { 0 }, codes[8] = { 0 };
    int mods = 0;
    for (int i = 0; i < n - 1 && mods < 8; i++) {
        char *m = g_ascii_strdown(parts[i], -1);
        if (!strcmp(m, "ctrl") || !strcmp(m, "control")) { bits[mods] = WPE_MODIFIER_KEYBOARD_CONTROL; syms[mods] = 0xffe3; codes[mods] = 29; }
        else if (!strcmp(m, "shift")) { bits[mods] = WPE_MODIFIER_KEYBOARD_SHIFT; syms[mods] = 0xffe1; codes[mods] = 42; }
        else if (!strcmp(m, "alt")) { bits[mods] = WPE_MODIFIER_KEYBOARD_ALT; syms[mods] = 0xffe9; codes[mods] = 56; }
        else if (!strcmp(m, "meta") || !strcmp(m, "super") || !strcmp(m, "cmd")) { bits[mods] = WPE_MODIFIER_KEYBOARD_META; syms[mods] = 0xffeb; codes[mods] = 125; }
        else { g_free(m); g_strfreev(parts); return FALSE; }
        g_free(m);
        mods++;
    }
    const char *key = parts[n - 1];
    guint keysym = 0, code = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(key_names); i++) {
        if (!g_ascii_strcasecmp(key, key_names[i].name)) {
            keysym = key_names[i].keysym;
            code = key_names[i].code;
            break;
        }
    }
    if (!keysym && g_utf8_strlen(key, -1) == 1) {
        gunichar c = g_utf8_get_char(key);
        gboolean shift;
        if (mods && c < 0x80)
            c = g_ascii_tolower(c);
        code = c < 0x80 ? ascii_code(c, &shift) : 0;
        keysym = (c >= 0x20 && c < 0x7f) ? c : 0x01000000 + c;
    }
    if (!keysym || !send) {
        g_strfreev(parts);
        return keysym != 0;
    }
    for (int i = 0; i < mods; i++)
        modifier(TRUE, bits[i], syms[i], codes[i]);
    key_event(TRUE, keysym, code);
    key_event(FALSE, keysym, code);
    for (int i = mods - 1; i >= 0; i--)
        modifier(FALSE, bits[i], syms[i], codes[i]);
    g_strfreev(parts);
    return TRUE;
}

static void focus_view(void)
{
    wpe_view_focus_in(wpe_view());
}

/* ── commands ──────────────────────────────────────────────────────────────────────────────── */

static int timeout_arg(Request *r, int index, int fallback_ms)
{
    if (r->argc > index) {
        char *end;
        double seconds = g_ascii_strtod(r->argv[index], &end);
        if (end != r->argv[index] && seconds > 0)
            return (int)(seconds * 1000);
    }
    return fallback_ms;
}

static void cmd_open(Request *r)
{
    if (r->argc < 2) {
        reply(r, ST_USAGE, "usage: open URL [TIMEOUT_SECONDS]\n");
        return;
    }
    char *uri = normalize_uri(r->argv[1]);
    webkit_web_view_load_uri(browser.view, uri);
    g_free(uri);
    /* a load follows (unless the address only moves within the page): its start can take a while
     * when the page's process is started first */
    wait_for_load(r, timeout_arg(r, 2, 60000), 15000);
}

static void cmd_history(Request *r, const char *which)
{
    if (!strcmp(which, "back")) {
        if (!webkit_web_view_can_go_back(browser.view)) {
            reply(r, ST_FAILED, "no page to go back to\n");
            return;
        }
        webkit_web_view_go_back(browser.view);
    } else if (!strcmp(which, "forward")) {
        if (!webkit_web_view_can_go_forward(browser.view)) {
            reply(r, ST_FAILED, "no page to go forward to\n");
            return;
        }
        webkit_web_view_go_forward(browser.view);
    } else
        webkit_web_view_reload(browser.view);
    wait_for_load(r, timeout_arg(r, 1, 60000), 1500);
}

static void cmd_info(Request *r)
{
    const char *uri = webkit_web_view_get_uri(browser.view);
    const char *title = webkit_web_view_get_title(browser.view);
    int width = 0, height = 0;
    if (wpe_view())
        wpe_toplevel_get_size(wpe_view_get_toplevel(wpe_view()), &width, &height);
    replyf(r, ST_OK,
           "url: %s\ntitle: %s\nloading: %s (%.0f%%)\nback: %s\nforward: %s\nsize: %dx%d\n%s%s%s%s%s%s",
           uri ? uri : "about:blank", title ? title : "",
           browser.loading ? "yes" : "no", webkit_web_view_get_estimated_load_progress(browser.view) * 100,
           webkit_web_view_can_go_back(browser.view) ? "yes" : "no",
           webkit_web_view_can_go_forward(browser.view) ? "yes" : "no", width, height,
           browser.last_load_error ? "error: " : "", browser.last_load_error ? browser.last_load_error : "",
           browser.last_load_error ? "\n" : "",
           browser.last_dialog ? "last dialog: " : "", browser.last_dialog ? browser.last_dialog : "",
           browser.last_dialog ? "\n" : "");
}

static void cmd_eval(Request *r)
{
    if (r->argc < 2) {
        reply(r, ST_USAGE, "usage: eval JAVASCRIPT\n");
        return;
    }
    JSCall *call = g_new0(JSCall, 1);
    call->r = r;
    call->done = reply_js_text;
    webkit_web_view_evaluate_javascript(browser.view, r->argv[1], -1, NULL, NULL, NULL, eval_finished, call);
}

static void cmd_read(Request *r)
{
    int max = 40000;
    gboolean hidden = FALSE;
    for (int i = 1; i < r->argc; i++) {
        if (!strcmp(r->argv[i], "--all") || !strcmp(r->argv[i], "--hidden"))
            hidden = TRUE;
        else if (atoi(r->argv[i]) > 0)
            max = atoi(r->argv[i]);
    }
    /* a number: WebKit passes no booleans to JavaScript (numbers, strings, dictionaries only) */
    char *body = g_strconcat("return (", snapshot_js, ")(maxChars, !!includeHidden);", NULL);
    call_js(r, body, args_dict("maxChars", g_variant_new_int32(max), "includeHidden", g_variant_new_int32(hidden), NULL), reply_js_text);
    g_free(body);
}

static void cmd_text(Request *r)
{
    call_js(r, "return document.body ? document.body.innerText : '';", NULL, reply_js_text);
}

static void cmd_html(Request *r)
{
    call_js(r, "const e = selector ? document.querySelector(selector) : document.documentElement; "
               "return e ? e.outerHTML : null;",
            args_dict("selector", g_variant_new_string(r->argc > 1 ? r->argv[1] : ""), NULL), reply_js_text);
}

static void cmd_links(Request *r)
{
    call_js(r, "const seen = new Set(); const lines = [];"
               "for (const a of document.querySelectorAll('a[href]')) {"
               "  if (a.href.startsWith('javascript:') || seen.has(a.href)) continue; seen.add(a.href);"
               "  const text = (a.innerText || a.getAttribute('aria-label') || a.title || '').replace(/\\s+/g, ' ').trim();"
               "  lines.push((text ? text.slice(0, 100) + '  ' : '') + a.href); }"
               "return lines.join('\\n');",
            NULL, reply_js_text);
}

/* click / hover / type / select: find the target (target.js), then act on its coordinates */
typedef enum { ACT_CLICK, ACT_DOUBLE_CLICK, ACT_RIGHT_CLICK, ACT_HOVER, ACT_TYPE, ACT_SELECT } Action;

typedef struct {
    Action action;
} TargetAction;

static void act_at(Request *r, Action action, double x, double y, const char *what);

static Action action_of(Request *r)
{
    const char *cmd = r->argv[0];
    if (!strcmp(cmd, "dblclick"))
        return ACT_DOUBLE_CLICK;
    if (!strcmp(cmd, "rightclick"))
        return ACT_RIGHT_CLICK;
    if (!strcmp(cmd, "hover"))
        return ACT_HOVER;
    if (!strcmp(cmd, "type"))
        return ACT_TYPE;
    if (!strcmp(cmd, "select"))
        return ACT_SELECT;
    return ACT_CLICK;
}

static void target_found(Request *r, JSCValue *value, GError *error)
{
    if (error) {
        replyf(r, ST_FAILED, "JavaScript error: %s\n", error->message);
        return;
    }
    if (!value || jsc_value_is_null(value) || jsc_value_is_undefined(value)) {
        const char *target = r->argv[0][0] == 't' ? r->argv[2] : r->argv[1];
        replyf(r, ST_NOT_FOUND, "nothing matches \"%s\" (a number from `web read`, a CSS selector or a link's or button's text)\n", target);
        return;
    }
    char *json = jsc_value_to_string(value);
    /* the fields we need, read without a JSON library: "x":N,"y":N,"tag":"...","text":"..." */
    double x = 0, y = 0;
    const char *p = strstr(json, "\"x\":");
    if (p)
        x = g_ascii_strtod(p + 4, NULL);
    p = strstr(json, "\"y\":");
    if (p)
        y = g_ascii_strtod(p + 4, NULL);
    act_at(r, action_of(r), x, y, json);
    g_free(json);
}

static void select_done(Request *r, JSCValue *value, GError *error)
{
    if (error) {
        replyf(r, ST_FAILED, "JavaScript error: %s\n", error->message);
        return;
    }
    char *text = js_value_text(value);
    if (!strcmp(text, "null"))
        replyf(r, ST_NOT_FOUND, "no option \"%s\"\n", r->argv[2]);
    else
        replyf(r, ST_OK, "selected %s\n", text);
    g_free(text);
}

static void send_click(Request *r)
{
    pointer_click(r->x, r->y, 1, 1);
}

static void send_double_click(Request *r)
{
    pointer_click(r->x, r->y, 1, 2);
}

static void send_right_click(Request *r)
{
    pointer_click(r->x, r->y, 3, 1);
}

static void wait_after_click(Request *r)
{
    /* a click that navigates answers when the new page is there */
    wait_for_load(r, timeout_arg(r, 3, 30000), 800);
}

/* the text of `type` (argv[1]) as key events */
static void send_text(Request *r)
{
    for (const char *c = r->argv[1]; *c; c = g_utf8_next_char(c))
        type_char(g_utf8_get_char(c));
}

static void typed(Request *r)
{
    long n = g_utf8_strlen(r->argv[1], -1);
    if (r->what)
        replyf(r, ST_OK, "typed %ld characters into %s\n", n, r->what);
    else
        replyf(r, ST_OK, "typed %ld characters\n", n);
}

static void send_typed_text(Request *r)
{
    input_send(r, send_text, typed);
}

static void act_at(Request *r, Action action, double x, double y, const char *what)
{
    focus_view();
    switch (action) {
    case ACT_HOVER:
        pointer_move(x, y);
        replyf(r, ST_OK, "hovering %s\n", what);
        return;
    case ACT_SELECT:
        pointer_click(x, y, 1, 1);
        call_js(r, "const t = /^\\d+$/.test(target) ? document.querySelector(`[data-wpe-ref=\"${target}\"]`) : document.querySelector(target);"
                   "if (!t || t.tagName !== 'SELECT') return null;"
                   "const want = value.trim().toLowerCase();"
                   "const o = Array.from(t.options).find(o => o.value === value || o.text.trim().toLowerCase() === want)"
                   "  || Array.from(t.options).find(o => o.text.trim().toLowerCase().includes(want));"
                   "if (!o) return null; t.value = o.value;"
                   "t.dispatchEvent(new Event('input', { bubbles: true })); t.dispatchEvent(new Event('change', { bubbles: true }));"
                   "return o.text.trim();",
                args_dict("target", g_variant_new_string(r->argv[1]), "value", g_variant_new_string(r->argv[2]), NULL), select_done);
        return;
    case ACT_TYPE:
        /* the click first, and the keys once it has focused the field */
        r->x = x;
        r->y = y;
        r->what = g_strdup(what);
        input_send(r, send_click, send_typed_text);
        return;
    default:
        break;
    }
    r->x = x;
    r->y = y;
    r->done_text = g_strdup_printf("clicked %s\n", what);
    input_send(r, action == ACT_RIGHT_CLICK ? send_right_click : action == ACT_DOUBLE_CLICK ? send_double_click : send_click,
               wait_after_click);
}

static gboolean parse_point(Request *r, int index, double *x, double *y)
{
    if (r->argc <= index + 1)
        return FALSE;
    char *end1, *end2;
    *x = g_ascii_strtod(r->argv[index], &end1);
    *y = g_ascii_strtod(r->argv[index + 1], &end2);
    return *end1 == 0 && *end2 == 0 && end1 != r->argv[index] && end2 != r->argv[index + 1];
}

static void cmd_target(Request *r)
{
    Action action = action_of(r);
    int target_index = action == ACT_TYPE ? 2 : 1;
    if (action == ACT_TYPE && r->argc == 2) {
        /* into whatever has the focus */
        focus_view();
        input_send(r, send_text, typed);
        return;
    }
    if (r->argc <= target_index || (action == ACT_SELECT && r->argc < 3)) {
        reply(r, ST_USAGE, action == ACT_TYPE ? "usage: type TEXT [TARGET]\n"
                           : action == ACT_SELECT ? "usage: select TARGET VALUE\n"
                           : "usage: click|dblclick|rightclick|hover TARGET | X Y\n");
        return;
    }
    double x, y;
    if (action != ACT_TYPE && action != ACT_SELECT && parse_point(r, 1, &x, &y)) {
        char *what = g_strdup_printf("(%g, %g)", x, y);
        act_at(r, action, x, y, what);
        g_free(what);
        return;
    }
    char *body = g_strconcat("return (", target_js, ")(target);", NULL);
    call_js(r, body, args_dict("target", g_variant_new_string(r->argv[target_index]), NULL), target_found);
    g_free(body);
}

static void send_keys(Request *r)
{
    for (int i = 1; i < r->argc; i++)
        key_spec(r->argv[i], TRUE);
}

static void wait_after_keys(Request *r)
{
    wait_for_load(r, 30000, 600); /* Enter in a form may navigate */
}

static void cmd_key(Request *r)
{
    if (r->argc < 2) {
        reply(r, ST_USAGE, "usage: key KEY... (Enter, Tab, Escape, ArrowDown, ctrl+a, shift+Tab, ...)\n");
        return;
    }
    for (int i = 1; i < r->argc; i++) {
        if (!key_spec(r->argv[i], FALSE)) {
            replyf(r, ST_USAGE, "unknown key \"%s\"\n", r->argv[i]);
            return;
        }
    }
    focus_view();
    r->done_text = g_strdup_printf("pressed %d key%s\n", r->argc - 1, r->argc > 2 ? "s" : "");
    input_send(r, send_keys, wait_after_keys);
}

static void cmd_scroll(Request *r)
{
    const char *how = r->argc > 1 ? r->argv[1] : "down";
    if (!strcmp(how, "top") || !strcmp(how, "bottom")) {
        call_js(r, !strcmp(how, "top") ? "window.scrollTo(0, 0); return `at ${scrollY}`;"
                                       : "window.scrollTo(0, document.documentElement.scrollHeight); return `at ${scrollY}`;",
                NULL, reply_js_text);
        return;
    }
    int notches = 3;
    if (r->argc > 2 && atoi(r->argv[2]) > 0)
        notches = atoi(r->argv[2]);
    double dx = 0, dy = 0;
    if (!strcmp(how, "down")) dy = -notches;
    else if (!strcmp(how, "up")) dy = notches;
    else if (!strcmp(how, "left")) dx = notches;
    else if (!strcmp(how, "right")) dx = -notches;
    else {
        reply(r, ST_USAGE, "usage: scroll [down|up|left|right [NOTCHES]] | top | bottom\n");
        return;
    }
    int width = 0, height = 0;
    wpe_toplevel_get_size(wpe_view_get_toplevel(wpe_view()), &width, &height);
    send_event(wpe_event_scroll_new(wpe_view(), WPE_INPUT_SOURCE_MOUSE, now_ms(), (WPEModifiers)browser.modifiers,
                                    dx, dy, FALSE, FALSE, width / 2.0, height / 2.0));
    replyf(r, ST_OK, "scrolled %s %d\n", how, notches);
}

/* wait load | text STRING | selector CSS | gone CSS [TIMEOUT] */
static void poll_check(Request *r);

static void poll_result(Request *r, JSCValue *value, GError *error)
{
    if (!error && value && jsc_value_to_boolean(value)) {
        char *state = page_state();
        replyf(r, ST_OK, "found\n%s", state);
        g_free(state);
        return;
    }
    if (g_get_monotonic_time() >= r->deadline) {
        replyf(r, ST_TIMEOUT, "not found in time: %s %s\n", r->poll_kind, r->poll_arg);
        return;
    }
    r->poll_id = g_timeout_add(300, (GSourceFunc)(void (*)(void))poll_check, r);
}

static void poll_check(Request *r)
{
    r->poll_id = 0;
    const char *body = !strcmp(r->poll_kind, "text") ? "return !!document.body && document.body.innerText.includes(arg);"
                     : !strcmp(r->poll_kind, "gone") ? "return !document.querySelector(arg);"
                     : "return !!document.querySelector(arg);";
    call_js(r, body, args_dict("arg", g_variant_new_string(r->poll_arg), NULL), poll_result);
}

static gboolean poll_check_source(gpointer data)
{
    poll_check(data);
    return G_SOURCE_REMOVE;
}

static void cmd_wait(Request *r)
{
    const char *what = r->argc > 1 ? r->argv[1] : "load";
    if (!strcmp(what, "load")) {
        if (!browser.loading) {
            char *state = page_state();
            replyf(r, ST_OK, "loaded\n%s", state);
            g_free(state);
            return;
        }
        wait_for_load(r, timeout_arg(r, 2, 60000), -1);
        return;
    }
    if ((!strcmp(what, "text") || !strcmp(what, "selector") || !strcmp(what, "gone")) && r->argc > 2) {
        r->poll_kind = g_strdup(what);
        r->poll_arg = g_strdup(r->argv[2]);
        r->deadline = g_get_monotonic_time() + (gint64)timeout_arg(r, 3, 30000) * 1000;
        r->poll_id = g_idle_add(poll_check_source, r);
        return;
    }
    reply(r, ST_USAGE, "usage: wait [load | text STRING | selector CSS | gone CSS] [TIMEOUT_SECONDS]\n");
}

static void cmd_resize(Request *r)
{
    int w, h;
    if (r->argc < 2 || sscanf(r->argv[1], "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
        reply(r, ST_USAGE, "usage: resize WIDTHxHEIGHT\n");
        return;
    }
    wpe_toplevel_resize(wpe_view_get_toplevel(wpe_view()), w, h);
    replyf(r, ST_OK, "asked for %dx%d\n", w, h);
}

static gboolean quit_soon(gpointer data)
{
    (void)data;
    g_main_loop_quit(browser.loop);
    return G_SOURCE_REMOVE;
}

static void run_request(Request *r)
{
    if (!r->argc) {
        reply(r, ST_USAGE, "empty request\n");
        return;
    }
    const char *cmd = r->argv[0];
    if (!strcmp(cmd, "open") || !strcmp(cmd, "goto"))
        cmd_open(r);
    else if (!strcmp(cmd, "back") || !strcmp(cmd, "forward") || !strcmp(cmd, "reload"))
        cmd_history(r, cmd);
    else if (!strcmp(cmd, "stop")) {
        webkit_web_view_stop_loading(browser.view);
        reply(r, ST_OK, "stopped\n");
    } else if (!strcmp(cmd, "info") || !strcmp(cmd, "status"))
        cmd_info(r);
    else if (!strcmp(cmd, "url")) {
        const char *uri = webkit_web_view_get_uri(browser.view);
        replyf(r, ST_OK, "%s\n", uri ? uri : "about:blank");
    } else if (!strcmp(cmd, "title")) {
        const char *title = webkit_web_view_get_title(browser.view);
        replyf(r, ST_OK, "%s\n", title ? title : "");
    } else if (!strcmp(cmd, "eval") || !strcmp(cmd, "js"))
        cmd_eval(r);
    else if (!strcmp(cmd, "read") || !strcmp(cmd, "snapshot"))
        cmd_read(r);
    else if (!strcmp(cmd, "text"))
        cmd_text(r);
    else if (!strcmp(cmd, "html"))
        cmd_html(r);
    else if (!strcmp(cmd, "links"))
        cmd_links(r);
    else if (!strcmp(cmd, "click") || !strcmp(cmd, "dblclick") || !strcmp(cmd, "rightclick") || !strcmp(cmd, "hover")
             || !strcmp(cmd, "type") || !strcmp(cmd, "select"))
        cmd_target(r);
    else if (!strcmp(cmd, "key") || !strcmp(cmd, "press"))
        cmd_key(r);
    else if (!strcmp(cmd, "scroll"))
        cmd_scroll(r);
    else if (!strcmp(cmd, "wait"))
        cmd_wait(r);
    else if (!strcmp(cmd, "resize"))
        cmd_resize(r);
    else if (!strcmp(cmd, "close") || !strcmp(cmd, "quit")) {
        reply(r, ST_OK, "closing\n");
        g_idle_add(quit_soon, NULL);
    } else if (!strcmp(cmd, "ping"))
        reply(r, ST_OK, "pong\n");
    else
        replyf(r, ST_USAGE, "unknown command \"%s\"\n", cmd);
}

/* ── the control socket ────────────────────────────────────────────────────────────────────── */

/* Parses "N\n" + N × ("LEN\n" bytes): 1 complete, 0 more needed, -1 malformed */
static int parse_request(Request *r)
{
    const char *p = (const char *)r->input->data, *end = p + r->input->len;
    const char *nl = memchr(p, '\n', end - p);
    if (!nl)
        return r->input->len > 32 ? -1 : 0;
    long count = strtol(p, NULL, 10);
    if (count < 0 || count > 1000)
        return -1;
    p = nl + 1;
    GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
    for (long i = 0; i < count; i++) {
        nl = memchr(p, '\n', end - p);
        if (!nl) {
            g_ptr_array_free(words, TRUE);
            return 0;
        }
        long length = strtol(p, NULL, 10);
        if (length < 0 || length > 64 * 1024 * 1024) {
            g_ptr_array_free(words, TRUE);
            return -1;
        }
        p = nl + 1;
        if (end - p < length) {
            g_ptr_array_free(words, TRUE);
            return 0;
        }
        g_ptr_array_add(words, g_strndup(p, length));
        p += length;
    }
    g_ptr_array_add(words, NULL);
    r->argc = (int)count;
    r->argv = (char **)g_ptr_array_free(words, FALSE);
    return 1;
}

static void read_more(Request *r);

static void read_done(GObject *source, GAsyncResult *result, gpointer data)
{
    Request *r = data;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, NULL);
    gsize size = 0;
    const guint8 *chunk = bytes ? g_bytes_get_data(bytes, &size) : NULL;
    if (!size) {
        if (bytes)
            g_bytes_unref(bytes);
        request_free(r);
        return;
    }
    g_byte_array_append(r->input, chunk, size);
    g_bytes_unref(bytes);
    switch (parse_request(r)) {
    case 1:
        run_request(r);
        break;
    case 0:
        read_more(r);
        break;
    default:
        reply(r, ST_USAGE, "malformed request\n");
        break;
    }
}

static void read_more(Request *r)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(r->connection));
    g_input_stream_read_bytes_async(in, 65536, G_PRIORITY_DEFAULT, NULL, read_done, r);
}

static gboolean on_incoming(GSocketService *service, GSocketConnection *connection, GObject *source, gpointer data)
{
    (void)service; (void)source; (void)data;
    Request *r = g_new0(Request, 1);
    r->browser = &browser;
    r->connection = g_object_ref(connection);
    r->input = g_byte_array_new();
    read_more(r);
    return TRUE;
}

static gboolean start_control_socket(GError **error)
{
    const char *dir = g_getenv("COLLABO_WEB_DIR");
    char *socket_dir = g_strdup(dir && *dir ? dir : "/tmp/.collabo-web");
    g_mkdir_with_parents(socket_dir, 0700);
    browser.socket_path = g_strdup_printf("%s/%s.sock", socket_dir, browser.name);
    g_free(socket_dir);

    /* a socket nobody answers on is left over from a browser that is gone */
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *address = g_unix_socket_address_new(browser.socket_path);
    GSocketConnection *existing = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, NULL);
    g_object_unref(client);
    if (existing) {
        g_object_unref(existing);
        g_object_unref(address);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "a browser named \"%s\" is running already (%s)", browser.name, browser.socket_path);
        return FALSE;
    }
    unlink(browser.socket_path);

    browser.service = g_socket_service_new();
    g_socket_service_stop(browser.service); /* until start_serving */
    gboolean ok = g_socket_listener_add_address(G_SOCKET_LISTENER(browser.service), address, G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, error);
    g_object_unref(address);
    if (!ok)
        return FALSE;
    g_signal_connect(browser.service, "incoming", G_CALLBACK(on_incoming), NULL);
    return TRUE;
}

/* ── the page's own requests ───────────────────────────────────────────────────────────────── */

static WebKitWebView *on_create(WebKitWebView *view, WebKitNavigationAction *action, gpointer data)
{
    (void)data;
    /* one page: what would open a window opens here */
    webkit_web_view_load_request(view, webkit_navigation_action_get_request(action));
    return NULL;
}

static gboolean on_script_dialog(WebKitWebView *view, WebKitScriptDialog *dialog, gpointer data)
{
    (void)view; (void)data;
    const char *kinds[] = { "alert", "confirm", "prompt", "before unload" };
    WebKitScriptDialogType type = webkit_script_dialog_get_dialog_type(dialog);
    g_free(browser.last_dialog);
    browser.last_dialog = g_strdup_printf("%s: %s", type < 4 ? kinds[type] : "dialog", webkit_script_dialog_get_message(dialog));
    g_print("wpe-browser: %s\n", browser.last_dialog);
    /* nobody is there to answer: accept (confirm: OK, prompt: its default, leave the page) */
    if (type == WEBKIT_SCRIPT_DIALOG_CONFIRM || type == WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM)
        webkit_script_dialog_confirm_set_confirmed(dialog, TRUE);
    else if (type == WEBKIT_SCRIPT_DIALOG_PROMPT)
        webkit_script_dialog_prompt_set_text(dialog, webkit_script_dialog_prompt_get_default_text(dialog));
    return TRUE;
}

static gboolean on_permission_request(WebKitWebView *view, WebKitPermissionRequest *request, gpointer data)
{
    (void)view; (void)data;
    webkit_permission_request_deny(request);
    return TRUE;
}

static void on_title(WebKitWebView *view, GParamSpec *spec, gpointer data)
{
    (void)spec; (void)data;
    const char *title = webkit_web_view_get_title(view);
    if (wpe_view())
        wpe_toplevel_set_title(wpe_view_get_toplevel(wpe_view()), title && *title ? title : "web");
}

static void on_web_process_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer data)
{
    (void)data;
    browser.web_process_crashes++;
    g_free(browser.last_load_error);
    browser.last_load_error = g_strdup_printf("the page's process ended (%s); `web reload` loads it again",
                                              reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "out of memory" : "crashed");
    g_printerr("wpe-browser: %s\n", browser.last_load_error);
    browser.loading = FALSE;
    browser.load_generation++;  /* ends every wait, whenever it began */
    finish_load_waiters(ST_FAILED, browser.last_load_error);
    start_serving();
    (void)view;
}

static gboolean on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer data)
{
    (void)view; (void)data;
    if (type != WEBKIT_POLICY_DECISION_TYPE_RESPONSE)
        return FALSE;
    WebKitResponsePolicyDecision *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
    if (!webkit_response_policy_decision_is_mime_type_supported(response)) {
        webkit_policy_decision_download(decision);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_decide_destination(WebKitDownload *download, const char *suggested, gpointer data)
{
    (void)data;
    char *dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *base = g_path_get_basename(suggested && *suggested ? suggested : "download");
    char *path = g_build_filename(dir, base, NULL);
    webkit_download_set_destination(download, path);
    g_print("wpe-browser: downloading to %s\n", path);
    g_free(base);
    g_free(dir);
    g_free(path);
    return TRUE;
}

static void on_download_started(WebKitNetworkSession *session, WebKitDownload *download, gpointer data)
{
    (void)session; (void)data;
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_decide_destination), NULL);
}

static void on_close(WebKitWebView *view, gpointer data)
{
    (void)view; (void)data;
    /* window.close(): an agent's page is closed by the agent (web close) */
}

/* the canvas was closed (`gui close`) or the display went away */
static void on_view_closed(WPEView *view, gpointer data)
{
    (void)view; (void)data;
    g_main_loop_quit(browser.loop);
}

/* ── main ──────────────────────────────────────────────────────────────────────────────────── */

static void set_feature(WebKitSettings *settings, const char *identifier, gboolean enabled)
{
    WebKitFeatureList *features = webkit_settings_get_all_features();
    for (gsize i = 0; i < webkit_feature_list_get_length(features); i++) {
        WebKitFeature *feature = webkit_feature_list_get(features, i);
        if (!g_strcmp0(webkit_feature_get_identifier(feature), identifier)) {
            webkit_settings_set_feature_enabled(settings, feature, enabled);
            break;
        }
    }
    webkit_feature_list_unref(features);
}

static void usage(void)
{
    g_printerr("usage: wpe-browser [--name NAME] [--size WxH] [--ephemeral] [--data-dir DIR] [--ignore-tls-errors]\n"
               "                   [--user-agent UA] [URL]\n"
               "Usually started by the `web` command; `web help` says more.\n");
}

int wpe_browser_main(int argc, char **argv)
{
    const char *url = NULL, *size = NULL, *data_dir = NULL, *user_agent = g_getenv("WEB_USER_AGENT");
    gboolean ephemeral = FALSE, ignore_tls = FALSE;
    browser.name = g_strdup("web");
    debugging = g_strcmp0(g_getenv("WPE_BROWSER_DEBUG"), "1") == 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            g_free(browser.name);
            browser.name = g_strdup(argv[++i]);
        } else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            size = argv[++i];
        else if (!strcmp(argv[i], "--data-dir") && i + 1 < argc)
            data_dir = argv[++i];
        else if (!strcmp(argv[i], "--ephemeral"))
            ephemeral = TRUE;
        else if (!strcmp(argv[i], "--ignore-tls-errors"))
            ignore_tls = TRUE;
        else if (!strcmp(argv[i], "--user-agent") && i + 1 < argc)
            user_agent = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else
            url = argv[i];
    }

    g_set_prgname(browser.name);
    g_set_application_name(browser.name);
    /* the guest's trust store (with the sandbox's own CA for the hosts it adds secrets to) */
    if (!g_getenv("SSL_CERT_FILE") && g_file_test("/etc/ssl/cert.pem", G_FILE_TEST_EXISTS))
        g_setenv("SSL_CERT_FILE", "/etc/ssl/cert.pem", TRUE);

    GError *error = NULL;
    if (!start_control_socket(&error)) {
        g_printerr("wpe-browser: %s\n", error->message);
        return 1;
    }

    if (ephemeral)
        browser.session = webkit_network_session_new_ephemeral();
    else {
        char *data = data_dir ? g_strdup(data_dir) : g_build_filename(g_get_user_data_dir(), "collabo-web", browser.name, NULL);
        char *cache = g_build_filename(g_get_user_cache_dir(), "collabo-web", browser.name, NULL);
        browser.session = webkit_network_session_new(data, cache);
        g_free(data);
        g_free(cache);
    }
    if (ignore_tls)
        webkit_network_session_set_tls_errors_policy(browser.session, WEBKIT_TLS_ERRORS_POLICY_IGNORE);
    g_signal_connect(browser.session, "download-started", G_CALLBACK(on_download_started), NULL);

    /* Each page's process is a program of its own in the guest, with ~100 MB of memory or more:
     * no cache of processes for sites left (WebKit's default keeps one per site), and the two
     * pages before for `back` (the back/forward cache) */
    webkit_web_context_set_cache_model(webkit_web_context_get_default(), WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);

    WebKitSettings *settings = webkit_settings_new();
    /* the guest has no GPU: Skia on the CPU, buffers in shared memory (WPE has no API for the
     * policy GTK has: the internal feature it sets; the web process also takes the CPU path by
     * itself when there is no EGL: patch 0004) */
    set_feature(settings, "HardwareAccelerationEnabled", FALSE);
    webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, TRUE);
    webkit_settings_set_enable_webgl(settings, FALSE);
    if (user_agent && *user_agent)
        webkit_settings_set_user_agent(settings, user_agent);
    else {
        /* WebKit names the machine as uname does: "Linux wasm" is a platform no site knows, and some
         * take it for a robot; the desktop Linux most of them have seen instead */
        const char *standard = webkit_settings_get_user_agent(settings);
        char **parts = g_strsplit(standard, "Linux wasm32", -1);
        char *joined = g_strjoinv("Linux x86_64", parts);
        g_strfreev(parts);
        parts = g_strsplit(joined, "Linux wasm", -1);
        g_free(joined);
        joined = g_strjoinv("Linux x86_64", parts);
        g_strfreev(parts);
        webkit_settings_set_user_agent(settings, joined);
        g_free(joined);
    }

    browser.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                                "network-session", browser.session,
                                                "settings", settings,
                                                NULL));
    g_object_unref(settings);
    WebKitUserScript *counter = webkit_user_script_new(INPUT_COUNTER_JS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                       WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(browser.view), counter);
    webkit_user_script_unref(counter);
    g_signal_connect(browser.view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(browser.view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(browser.view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_errors), NULL);
    g_signal_connect(browser.view, "create", G_CALLBACK(on_create), NULL);
    g_signal_connect(browser.view, "script-dialog", G_CALLBACK(on_script_dialog), NULL);
    g_signal_connect(browser.view, "permission-request", G_CALLBACK(on_permission_request), NULL);
    g_signal_connect(browser.view, "notify::title", G_CALLBACK(on_title), NULL);
    g_signal_connect(browser.view, "web-process-terminated", G_CALLBACK(on_web_process_terminated), NULL);
    g_signal_connect(browser.view, "decide-policy", G_CALLBACK(on_decide_policy), NULL);
    g_signal_connect(browser.view, "close", G_CALLBACK(on_close), NULL);

    WPEView *view = wpe_view();
    if (view) {
        WPEToplevel *toplevel = wpe_view_get_toplevel(view);
        if (toplevel) {
            int w, h;
            if (size && sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
                wpe_toplevel_resize(toplevel, w, h);
            wpe_toplevel_set_title(toplevel, browser.name);
        }
        g_signal_connect(view, "closed", G_CALLBACK(on_view_closed), NULL);
        wpe_view_focus_in(view);
    }

    browser.wpe_view = view;
    char *start = normalize_uri(url ? url : "about:blank");
    webkit_web_view_load_uri(browser.view, start);
    g_free(start);
    /* a first page that does not end still lets the browser be driven */
    g_timeout_add_seconds(60, start_serving_anyway, NULL);

    browser.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(browser.loop);

    g_socket_service_stop(browser.service);
    unlink(browser.socket_path);
    /* the web and network processes end when their connections close */
    g_object_unref(browser.view);
    return 0;
}
