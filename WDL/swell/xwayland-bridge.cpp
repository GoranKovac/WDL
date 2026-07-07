#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif
#include "xwayland-bridge.h"

static pid_t    s_xwayland_pid = 0;
XWaylandWM     *g_wm           = nullptr;
Display *g_wm_dpy       = nullptr;

static int x11_error_handler(Display *dpy, XErrorEvent *err)
{
    if (err->error_code == BadWindow   ||
        err->error_code == BadDrawable ||
        err->error_code == BadMatch    ||
        err->error_code == BadAccess) return 0;
#ifdef _DEBUG
    char buf[256];
    XGetErrorText(dpy, err->error_code, buf, sizeof(buf));
    DEBUG_PRINT("X11 Error: %s (request %d, resource 0x%lx)\n",
                buf, err->request_code, err->resourceid);
#endif
    return 0;
}

// ─── State ───────────────────────────────────────────────────────────────────
// One plugin = one capture. We composite-capture the plugin's X11 window into a
// pixmap and blit it into the SWELL GtkWidget (draw area). Input is forwarded
// back to the plugin via XTest.

struct Capture {
    Display   *dpy         = nullptr;   // per-plugin connection to :10
    Window     parent_win  = 0;         // container we created on :10
    Window     plugin_win  = 0;         // the plugin's X11 window (child of parent)
    Window     gui_win     = 0;         // Wine child GUI (or == plugin_win for native)
    Pixmap     pixmap      = 0;         // composite backing pixmap of plugin_win
    Damage     damage      = 0;         // damage on parent_win (via g_wm_dpy)
    int        damage_base = 0;
    GtkWidget *widget      = nullptr;   // SWELL draw area we blit into
    HWND       hwnd        = nullptr;   // back-reference

    // ── Popups (plugin dropdown menus / override-redirect windows) ──
    // Drawn into a single transparent full-screen overlay canvas, exactly like
    // the proven old implementation: each popup is composite-captured and blitted
    // into the canvas at its screen position.
    GtkWidget *popup_canvas    = nullptr;
    GtkWidget *canvas_draw     = nullptr;
    int        canvas_origin_x = 0;
    int        canvas_origin_y = 0;
    int        canvas_w        = 0;
    int        canvas_h        = 0;
    int        gtk_x           = 0;     // plugin widget screen offset (X)
    int        gtk_y           = 0;     // plugin widget screen offset (Y)
    Window     root_popup      = 0;     // first popup in the chain
    struct PopupWin { Window x11_win; Pixmap pixmap; int x,y,w,h; bool visible; };
    std::vector<PopupWin> popups;
};

// Route :10 events (on g_wm_dpy) to the owning capture.
static std::map<Window, Capture*> g_captures;

static void register_capture(Capture *c)
{
    g_captures[c->parent_win] = c;
    g_captures[c->plugin_win] = c;
    if (c->gui_win) g_captures[c->gui_win] = c;
}
static void unregister_capture(Capture *c)
{
    g_captures.erase(c->parent_win);
    g_captures.erase(c->plugin_win);
    if (c->gui_win) g_captures.erase(c->gui_win);
}
static Capture* find_capture(Window w)
{
    auto it = g_captures.find(w);
    return it != g_captures.end() ? it->second : nullptr;
}

// Bridge instance stored on the SWELL HWND.
struct bridgeState {
    Display *disp   = nullptr;   // this plugin's connection to :10
    Window   parent = 0;         // container window on :10
    Capture *cap    = nullptr;
    bool     placed = false;     // has the SWELL widget been put in its container
};

// ─── Blit: plugin pixmap → SWELL widget ──────────────────────────────────────

static gboolean on_draw(GtkWidget *, cairo_t *cr, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy || !c->pixmap) return FALSE;

    Window root; int x, y; unsigned int w, h, border, depth;
    if (!XGetGeometry(c->dpy, c->pixmap, &root, &x, &y, &w, &h, &border, &depth))
        return FALSE;

    XWindowAttributes wa;
    Visual *visual = DefaultVisual(c->dpy, DefaultScreen(c->dpy));
    if (XGetWindowAttributes(c->dpy, c->plugin_win, &wa)) visual = wa.visual;

    cairo_surface_t *surf = cairo_xlib_surface_create(c->dpy, c->pixmap, visual, w, h);
    if (surf) {
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        cairo_surface_destroy(surf);
    }
    return TRUE;
}

// ─── Input forwarding ────────────────────────────────────────────────────────
// The plugin's window on :10 lives at some origin (its container's position on
// :10 plus its own offset within it). XTest wants root coordinates, so translate
// the widget-local event point through the plugin window's actual on-:10 origin.
// No hardcoded offset: XTranslateCoordinates gives the true origin every time.

static void forward_motion(Capture *c, int wx, int wy)
{
    Window child; int rx, ry;
    XTranslateCoordinates(c->dpy, c->plugin_win, DefaultRootWindow(c->dpy),
                          wx, wy, &rx, &ry, &child);
    XTestFakeMotionEvent(c->dpy, DefaultScreen(c->dpy), rx, ry, CurrentTime);
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return FALSE;
    if (c->hwnd) SetFocus(c->hwnd);
    if (!gtk_widget_has_focus(widget))
        gtk_widget_grab_focus(widget);
    forward_motion(c, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(c->dpy, e->button, True, CurrentTime);
    XFlush(c->dpy);
    return TRUE;
}

static gboolean on_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return FALSE;
    forward_motion(c, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(c->dpy, e->button, False, CurrentTime);
    XFlush(c->dpy);
    return TRUE;
}

static gboolean on_motion(GtkWidget *, GdkEventMotion *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return FALSE;
    forward_motion(c, (int)e->x, (int)e->y);
    XFlush(c->dpy);
    return TRUE;
}

static gboolean on_scroll(GtkWidget *, GdkEventScroll *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return FALSE;
    unsigned int btn = 0;
    switch (e->direction) {
        case GDK_SCROLL_UP:    btn = 4; break;
        case GDK_SCROLL_DOWN:  btn = 5; break;
        case GDK_SCROLL_LEFT:  btn = 6; break;
        case GDK_SCROLL_RIGHT: btn = 7; break;
        default: return FALSE;
    }
    forward_motion(c, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(c->dpy, btn, True,  CurrentTime);
    XTestFakeButtonEvent(c->dpy, btn, False, CurrentTime);
    XFlush(c->dpy);
    return TRUE;
}

gboolean on_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
    Capture *c = (Capture*)data;
    DEBUG_PRINT("[GTK] enter widget=%p\n", widget);
    XRaiseWindow(c->dpy, c->parent_win);
    XFlush(c->dpy);
    return FALSE;
}

// Wire the SWELL draw area to blit + forward input.
static void connect_widget(Capture *c)
{
    if (!c->widget) return;
    gtk_widget_add_events(c->widget,
                          GDK_ENTER_NOTIFY_MASK |
                          GDK_LEAVE_NOTIFY_MASK |
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_SCROLL_MASK);

    g_signal_connect(c->widget, "enter-notify-event",   G_CALLBACK(on_enter), c);
    // g_signal_connect(c->widget, "leave-notify-event",   G_CALLBACK(on_leave), c);
    g_signal_connect(c->widget, "draw",                 G_CALLBACK(on_draw),           c);
    g_signal_connect(c->widget, "button-press-event",   G_CALLBACK(on_button_press),   c);
    g_signal_connect(c->widget, "button-release-event", G_CALLBACK(on_button_release), c);
    g_signal_connect(c->widget, "motion-notify-event",  G_CALLBACK(on_motion),         c);
    g_signal_connect(c->widget, "scroll-event",         G_CALLBACK(on_scroll),         c);
    gtk_widget_queue_draw(c->widget);
}

static Capture* setup_capture(Display *dpy, Window parent_win, Window plugin_win, HWND hwnd)
{
    Capture *c = new Capture();
    c->dpy        = dpy;
    c->parent_win = parent_win;
    c->plugin_win = plugin_win;
    c->hwnd       = hwnd;

    XCompositeRedirectWindow(dpy, plugin_win, CompositeRedirectAutomatic);

    int base, err;
    if (g_wm_dpy && XDamageQueryExtension(g_wm_dpy, &base, &err)) {
        c->damage      = XDamageCreate(g_wm_dpy, parent_win, XDamageReportNonEmpty);
        c->damage_base = base;
    }

    XFlush(dpy);

    c->pixmap = XCompositeNameWindowPixmap(dpy, plugin_win);

    register_capture(c);
    DEBUG_PRINT("[XW] setup parent=0x%lx plugin=0x%lx pixmap=0x%lx\n",
                parent_win, plugin_win, c->pixmap);
    return c;
}

static void refresh_pixmap(Capture *c)
{
    if (!c) return;
    if (c->pixmap) XFreePixmap(c->dpy, c->pixmap);
    c->pixmap = XCompositeNameWindowPixmap(c->dpy, c->plugin_win);
    if (c->widget) gtk_widget_queue_draw(c->widget);
}

static void cleanup_capture(Capture *c)
{
    if (!c) return;
    unregister_capture(c);
    if (c->damage) XDamageDestroy(g_wm_dpy, c->damage);
    if (c->pixmap) XFreePixmap(c->dpy, c->pixmap);
    Display *d = c->dpy;
    c->dpy = nullptr;
    if (d) XCloseDisplay(d);
    delete c;
}

// ─── Popup overlay canvas (ported from proven implementation) ────────────────
// A single transparent full-screen GTK_WINDOW_POPUP overlay. Each plugin popup
// (override-redirect menu) is composite-captured and blitted into the canvas at
// its screen position. Input on the canvas is forwarded back to the plugin.

static gboolean canvas_draw_cb(GtkWidget *, cairo_t *cr, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return FALSE;

    // Transparent clear.
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    for (const auto &p : c->popups) {
        if (!p.visible || p.pixmap == None) continue;
        Window rr; int gx, gy; unsigned int gw, gh, gb, gd;
        if (!XGetGeometry(c->dpy, p.pixmap, &rr, &gx, &gy, &gw, &gh, &gb, &gd)) continue;

        int draw_x = p.x - c->canvas_origin_x;
        int draw_y = p.y - c->canvas_origin_y;

        XWindowAttributes wa;
        Visual *visual = DefaultVisual(c->dpy, DefaultScreen(c->dpy));
        if (XGetWindowAttributes(c->dpy, p.x11_win, &wa)) visual = wa.visual;

        cairo_surface_t *surf = cairo_xlib_surface_create(c->dpy, p.pixmap, visual, gw, gh);
        if (surf) {
            cairo_set_source_surface(cr, surf, draw_x, draw_y);
            cairo_paint(cr);
            cairo_surface_destroy(surf);
        }
    }
    return TRUE;
}

static gboolean canvas_button_press(GtkWidget *, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return FALSE;

    int screen_x = (int)e->x + c->canvas_origin_x;
    int screen_y = (int)e->y + c->canvas_origin_y;
    int x11_x = screen_x - c->gtk_x;
    int x11_y = screen_y - c->gtk_y;

    bool hit = false;
    for (auto it = c->popups.rbegin(); it != c->popups.rend(); ++it) {
        if (!it->visible) continue;
        if (screen_x >= it->x && screen_x < it->x + it->w &&
            screen_y >= it->y && screen_y < it->y + it->h) {
            hit = true;
            XTestFakeMotionEvent(c->dpy, DefaultScreen(c->dpy), x11_x, x11_y, CurrentTime);
            XFlush(c->dpy);
            XTestFakeButtonEvent(c->dpy, e->button, True, CurrentTime);
            XFlush(c->dpy);
            return TRUE;
        }
    }
    // Clicked outside any popup — dismiss.
    if (!hit) {
        if (c->popup_canvas) gtk_widget_hide(c->popup_canvas);
        XTestFakeButtonEvent(c->dpy, e->button, True,  CurrentTime);
        XFlush(c->dpy);
        XTestFakeButtonEvent(c->dpy, e->button, False, CurrentTime);
        XFlush(c->dpy);
        return TRUE;
    }
    return FALSE;
}

static gboolean canvas_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return FALSE;
    XTestFakeButtonEvent(c->dpy, e->button, False, CurrentTime);
    XFlush(c->dpy);
    return TRUE;
}

static gboolean canvas_motion(GtkWidget *, GdkEventMotion *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return FALSE;

    int screen_x = (int)e->x + c->canvas_origin_x;
    int screen_y = (int)e->y + c->canvas_origin_y;
    int x11_x = screen_x - c->gtk_x;
    int x11_y = screen_y - c->gtk_y;

    XTestFakeMotionEvent(c->dpy, DefaultScreen(c->dpy), x11_x, x11_y, CurrentTime);
    XFlush(c->dpy);

    static guint32 last_time = 0;
    guint32 now = g_get_monotonic_time() / 1000;
    if (now - last_time < 16) return TRUE;
    last_time = now;

    for (auto &p : c->popups) {
        if (!p.visible) continue;
        if (screen_x >= p.x && screen_x < p.x + p.w &&
            screen_y >= p.y && screen_y < p.y + p.h) {
            if (p.pixmap != None) XFreePixmap(c->dpy, p.pixmap);
            p.pixmap = XCompositeNameWindowPixmap(c->dpy, p.x11_win);
            if (c->canvas_draw) gtk_widget_queue_draw(c->canvas_draw);
            break;
        }
    }
    return TRUE;
}

static void create_popup_canvas(Capture *c)
{
    if (c->popup_canvas) return;

    GtkWidget *win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);

    GtkWidget *top = c->widget ? gtk_widget_get_toplevel(c->widget) : nullptr;
    if (top && GTK_IS_WINDOW(top))
        gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(top));

    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_modal(GTK_WINDOW(win), FALSE);

    GdkScreen *screen = gdk_screen_get_default();
    int sw = gdk_screen_get_width(screen);
    int sh = gdk_screen_get_height(screen);
    c->canvas_origin_x = 0;
    c->canvas_origin_y = 0;
    c->canvas_w = sw;
    c->canvas_h = sh;

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, sw, sh);
    gtk_container_add(GTK_CONTAINER(win), da);

    g_signal_connect(da, "draw",                 G_CALLBACK(canvas_draw_cb),        c);
    gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(da, "button-press-event",   G_CALLBACK(canvas_button_press),   c);
    g_signal_connect(da, "button-release-event", G_CALLBACK(canvas_button_release), c);
    g_signal_connect(da, "motion-notify-event",  G_CALLBACK(canvas_motion),         c);

    c->popup_canvas = win;
    c->canvas_draw  = da;
}

static void canvas_add_popup(Capture *c, Window x11_win, XWindowAttributes *attr)
{
    if (!c) return;

    Window child; int px, py;
    XTranslateCoordinates(c->dpy, c->gui_win ? c->gui_win : c->plugin_win,
                          DefaultRootWindow(c->dpy), 0, 0, &px, &py, &child);

    if (!c->popup_canvas) create_popup_canvas(c);

    XCompositeRedirectWindow(c->dpy, x11_win, CompositeRedirectAutomatic);
    XSync(c->dpy, False);
    Pixmap pixmap = XCompositeNameWindowPixmap(c->dpy, x11_win);

    Capture::PopupWin p;
    p.x11_win = x11_win;
    p.pixmap  = pixmap;
    p.x = px + attr->x + c->gtk_x;
    p.y = py + attr->y + c->gtk_y;
    p.w = attr->width;
    p.h = attr->height;
    p.visible = true;
    c->popups.push_back(p);

    if (c->root_popup == None) c->root_popup = x11_win;

    if (!gtk_widget_get_visible(c->popup_canvas))
        gtk_widget_show_all(c->popup_canvas);
    if (c->canvas_draw) gtk_widget_queue_draw(c->canvas_draw);
}

static void canvas_remove_popup(Capture *c, Window x11_win)
{
    if (!c) return;
    auto it = std::find_if(c->popups.begin(), c->popups.end(),
                           [x11_win](const Capture::PopupWin &p){ return p.x11_win == x11_win; });
    if (it != c->popups.end()) {
        c->popups.erase(it);
    }
    if (c->root_popup == x11_win) c->root_popup = None;

    if (c->popups.empty()) {
        if (c->popup_canvas) {
            gtk_widget_destroy(c->popup_canvas);
            c->popup_canvas = nullptr;
            c->canvas_draw  = nullptr;
        }
    } else if (c->canvas_draw) {
        gtk_widget_queue_draw(c->canvas_draw);
    }
}

// ─── :10 event loop (on the GLib main loop, main thread) ─────────────────────

static gboolean wm_event_cb(GIOChannel *, GIOCondition, gpointer)
{
    while (XPending(g_wm_dpy))
    {
        XEvent ev;
        XNextEvent(g_wm_dpy, &ev);

        // Let the ICCCM WM layer handle MapRequest/ConfigureRequest/etc first.
        if (g_wm) g_wm->handle_event(&ev);

        // Popup detection: an override-redirect window mapped on :10 that isn't
        // one of a capture's own windows is a plugin popup (dropdown menu).
        if (ev.type == MapNotify) {
            Window w = ev.xmap.window;
            XWindowAttributes attr;
            if (XGetWindowAttributes(g_wm_dpy, w, &attr) &&
                attr.override_redirect && attr.map_state == IsViewable &&
                attr.width > 1 && attr.height > 1 &&
                attr.width != 12 && attr.height != 12 &&
                !find_capture(w))
            {
                // Attribute the popup to the currently-active capture. With one
                // plugin interacting at a time, that's the most recent capture
                // whose canvas/root is relevant; use the capture that owns the
                // plugin the popup belongs to (the only one, in practice).
                Capture *owner = nullptr;
                for (auto &kv : g_captures) { owner = kv.second; break; }
                if (owner) {
                    // We need this capture's own connection (owner->dpy) to
                    // composite the popup; but the event came on g_wm_dpy. The
                    // popup window id is valid on both connections.
                    canvas_add_popup(owner, w, &attr);
                    continue;
                }
            }
        }
        else if (ev.type == UnmapNotify) {
            Window w = ev.xunmap.window;
            for (auto &kv : g_captures) {
                Capture *cc = kv.second;
                auto it = std::find_if(cc->popups.begin(), cc->popups.end(),
                    [w](const Capture::PopupWin &p){ return p.x11_win == w; });
                if (it != cc->popups.end()) { canvas_remove_popup(cc, w); break; }
            }
        }

        Capture *c = find_capture(ev.xany.window);
        if (!c) continue;

        // Damage → re-grab the pixmap and redraw.
        if (c->damage_base && ev.type == c->damage_base + XDamageNotify) {
            XDamageSubtract(c->dpy, c->damage, None, None);
            refresh_pixmap(c);
            continue;
        }

        switch (ev.type)
        {
            case ConfigureNotify:
                if (ev.xconfigure.window == c->plugin_win || ev.xconfigure.window == c->gui_win)
                {
                    XResizeWindow(c->dpy, c->parent_win,
                                  ev.xconfigure.width, ev.xconfigure.height);
                    XFlush(c->dpy);
                    refresh_pixmap(c);
                    if (c->hwnd) SendMessage(c->hwnd, WM_SIZE, SIZE_RESTORED, 0);
                }
                break;

            case MapNotify:
                if (ev.xmap.window == c->plugin_win || ev.xmap.window == c->gui_win) {
                    XSync(c->dpy, False);
                    refresh_pixmap(c);
                }
                break;
            case Expose:
                refresh_pixmap(c);
                break;
            default:
                break;
        }
    }
    return TRUE;
}

void init_private_xwayland()
{
    if (s_xwayland_pid > 0) return;

    s_xwayland_pid = fork();
    if (s_xwayland_pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        execl("/usr/bin/Xwayland", "Xwayland", ":10", "-rootless", NULL);
        exit(1);
    }
    if (s_xwayland_pid < 0) return;

    setenv("DISPLAY", ":10", 1);
    XInitThreads();
    XSetErrorHandler(x11_error_handler);

    for (int i = 0; i < 30; i++) {
        g_wm_dpy = XOpenDisplay(":10");
        if (g_wm_dpy) break;
        usleep(100000);
    }
    if (!g_wm_dpy) { DEBUG_PRINT("[XW] Xwayland not ready\n"); return; }

    g_wm = new XWaylandWM(g_wm_dpy);
    Window support = XCreateSimpleWindow(g_wm_dpy, DefaultRootWindow(g_wm_dpy),
                                         -2, -2, 1, 1, 0, 0, 0);
    XMapWindow(g_wm_dpy, support);
    g_wm->announce_wm(support);

    GIOChannel *ch = g_io_channel_unix_new(ConnectionNumber(g_wm_dpy));
    g_io_add_watch(ch, G_IO_IN, wm_event_cb, nullptr);
    g_io_channel_unref(ch);

    DEBUG_PRINT("[XW] initialised\n");
}

static gboolean capture_update(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return TRUE;
    bridgeState *bs = (bridgeState*)hwnd->m_private_data;

    // First tick(s): the plugin creates its window as a child of our container.
    if (!bs->cap && bs->disp && bs->parent)
    {
        Window root, par, *list = nullptr; unsigned int n = 0;
        if (XQueryTree(bs->disp, bs->parent, &root, &par, &list, &n) && list && n)
        {
            Window plugin_win = list[0];
            XFree(list);

            XWindowAttributes attr;
            if (XGetWindowAttributes(bs->disp, plugin_win, &attr))
                XResizeWindow(bs->disp, bs->parent, attr.width, attr.height);
            XFlush(bs->disp);

            Capture *c = setup_capture(bs->disp, bs->parent, plugin_win, hwnd);
            c->widget = hwnd->m_oswidget;

            // Wine plugins nest a child GUI window; native plugins draw directly.
            Window gr, gp, *gk = nullptr; unsigned int gn = 0;
            if (XQueryTree(bs->disp, plugin_win, &gr, &gp, &gk, &gn) && gn) {
                c->gui_win = gk[0];
                g_captures[c->gui_win] = c;
                XFree(gk);
            } else {
                c->gui_win = plugin_win;
            }

            connect_widget(c);
            bs->cap = c;
        }
        else if (list) XFree(list);
    }

    if (bs->cap && bs->cap->widget)
        gtk_widget_queue_draw(bs->cap->widget);

    return TRUE;
}

void xw_destroy(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return;
    bridgeState *bs = (bridgeState*)hwnd->m_private_data;
    if (hwnd->m_oswidget) {
        GtkWidget *parent = gtk_widget_get_parent(hwnd->m_oswidget);
        if (parent) gtk_container_remove(GTK_CONTAINER(parent), hwnd->m_oswidget);
    }
    if (bs->cap) cleanup_capture(bs->cap);
    hwnd->m_private_data = 0;
    delete bs;
}

// Place the SWELL draw area inside its parent container, at the SWELL-computed
// position, and size it. Works for both floating (own window) and list-embedded.
void xw_size(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data || !hwnd->m_oswidget) return;
    bridgeState *bs = (bridgeState*)hwnd->m_private_data;

    HWND parent = hwnd->m_parent;
    while (parent && !parent->m_oswidget) parent = parent->m_parent;
    if (!parent || !parent->m_oswidget) return;

    GtkWidget *container = parent->m_oswidget;
    if (GTK_IS_WINDOW(container)) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(container));
        if (child) container = child;
        else {
            GtkWidget *fixed = gtk_fixed_new();
            gtk_container_add(GTK_CONTAINER(container), fixed);
            gtk_widget_show(fixed);
            container = fixed;
        }
    }

    RECT r = hwnd->m_position;
    int pos_x = 0, pos_y = 0;
    HWND fp = hwnd->m_parent;
    if (fp) {
        // FLOATING PLUGIN
        DEBUG_PRINT("[XWM] xw_size: parent->m_position=%d,%d,%d,%d\n",
                    fp->m_position.left, fp->m_position.top,
                    fp->m_position.right, fp->m_position.bottom);
        pos_x = fp->m_position.left + r.left;
        pos_y = fp->m_position.top  + r.top;
        // PLUGIN IN FX LIST
        if (fp->m_parent) {
            DEBUG_PRINT("[XWM] xw_size: grandparent->m_position=%d,%d,%d,%d\n",
                        fp->m_parent->m_position.left, fp->m_parent->m_position.top,
                        fp->m_parent->m_position.right, fp->m_parent->m_position.bottom);
            pos_y += fp->m_parent->m_position.bottom - fp->m_position.bottom;
        }
    }
    int w = r.right - r.left, h = r.bottom - r.top;

    // Feed the same GTK offset to the popup canvas so popups position correctly
    // in both floating and FX-list embedded modes.
    if (bs->cap) { bs->cap->gtk_x = pos_x; bs->cap->gtk_y = pos_y; }

    if (GTK_IS_FIXED(container)) {
        if (!bs->placed) {
            gtk_fixed_put(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
            gtk_widget_set_size_request(hwnd->m_oswidget, w, h);
            gtk_widget_show(hwnd->m_oswidget);
            bs->placed = true;
        } else {
            gtk_fixed_move(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
            gtk_widget_set_size_request(hwnd->m_oswidget, w, h);
        }
    } else if (GTK_IS_CONTAINER(container) && !bs->placed) {
        gtk_container_add(GTK_CONTAINER(container), hwnd->m_oswidget);
        gtk_widget_show(hwnd->m_oswidget);
        bs->placed = true;
    }
}

static LRESULT xw_bridgeProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
        case WM_DESTROY: xw_destroy(hwnd); break;
        case WM_TIMER: capture_update(hwnd); break;
        case WM_MOVE:
        case WM_SIZE: xw_size(hwnd); break;
    }
    return 0;
}

HWND xw_bridge_create(HWND viewpar, void **wref, const RECT *r, const char *bridge_class_name)
{
    HWND hwnd = nullptr;
    *wref = nullptr;

    Display *disp = XOpenDisplay(":10");
    if (!disp) {
        hwnd = new HWND__(viewpar, 0, r, NULL, false, NULL);
        hwnd->m_classname = bridge_class_name;
        return hwnd;
    }

    int screen = DefaultScreen(disp);
    Window root = RootWindow(disp, screen);
    int w = wdl_max(r->right - r->left, 1);
    int h = wdl_max(r->bottom - r->top, 1);

    Window container = XCreateSimpleWindow(disp, root, 0, 0, w, h, 0,
                                           BlackPixel(disp, screen),
                                           WhitePixel(disp, screen));
    XMapWindow(disp, container);
    XFlush(disp);

    GtkWidget *draw_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area, w, h);

    hwnd = new HWND__(viewpar, 0, r, NULL, true, xw_bridgeProc);
    hwnd->m_classname = bridge_class_name;
    hwnd->m_oswidget  = draw_area;

    bridgeState *bs = new bridgeState();
    bs->disp   = disp;
    bs->parent = container;
    hwnd->m_private_data = (INT_PTR)bs;

    *wref = (void*)container;
    SetTimer(hwnd, 1, 16, NULL);
    return hwnd;
}

// ─── Public key forwarding (called from SWELL OnKeyEvent) ────────────────────
// Sends a key to :10, targeting the open popup (whose keyboard grab wedges
// REAPER when keys aren't delivered) or the main plugin GUI otherwise. SWELL
// consumes the key after calling this so REAPER does not also act on it.
bool xw_forward_key(HWND hwnd, int keycode, int state, bool is_press)
{
    if (!hwnd || !hwnd->m_private_data) return false;
    bridgeState *bs = (bridgeState*)hwnd->m_private_data;
    Capture *c = bs->cap;
    if (!c || !c->dpy) return false;

    Window target = (c->root_popup && !c->popups.empty()) ? c->root_popup
                  : (c->gui_win ? c->gui_win : c->plugin_win);
    if (!target) return false;

    XKeyEvent xev; memset(&xev, 0, sizeof(xev));
    xev.type        = is_press ? KeyPress : KeyRelease;
    xev.display     = c->dpy;
    xev.window      = target;
    xev.root        = DefaultRootWindow(c->dpy);
    xev.time        = CurrentTime;
    xev.keycode     = keycode;
    xev.state       = state;
    xev.same_screen = True;
    XSendEvent(c->dpy, target, True,
               is_press ? KeyPressMask : KeyReleaseMask, (XEvent*)&xev);
    XFlush(c->dpy);
    return true;
}
