#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif
#include "xwayland-bridge.h"

XWaylandWM     *g_wm           = nullptr;
Display *g_wm_dpy       = nullptr;

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
    GtkWidget *widget      = nullptr;   // SWELL draw area we blit into
    HWND       hwnd        = nullptr;   // back-reference
    //
    Damage     damage      = 0;         // damage on parent_win (via g_wm_dpy)
    int        damage_base = 0;

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

    // ── Modals (real dialog windows the plugin opens) ──
    struct ModalWin { Window x11_win; Pixmap pixmap; GtkWidget *gtk_win; GtkWidget *draw; Damage damage; };
    std::vector<ModalWin> modals;
};

// Route :10 events (on g_wm_dpy) to the owning capture.
static std::map<Window, Capture*> g_captures;

// Damage event base on g_wm_dpy (queried once). Same for every damage object we
// create, so we test event type against this rather than a per-capture copy —
// modal windows aren't in g_captures, so we can't resolve them before the type
// check.
static int g_damage_event_base = -1;

// Damage error base (XDamageQueryExtension), non-static so the WM's X error
// handler can ignore BadDamage teardown races. -1 until queried in init.
int g_bridge_damage_error_base = -1;

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

static bool on_draw(GtkWidget *, cairo_t *cr, gpointer data)
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
    return true;
}

static void forward_motion(Capture *c, int wx, int wy)
{
    Window child; int rx, ry;
    XTranslateCoordinates(c->dpy, c->plugin_win, DefaultRootWindow(c->dpy),
                          wx, wy, &rx, &ry, &child);
    XTestFakeMotionEvent(c->dpy, DefaultScreen(c->dpy), rx, ry, CurrentTime);
}

// Present + keep-above every open modal. Called when a click lands on a background
// window while a modal is up — with true modality this is rare, but if a grab
// leaked this pulls the modal back to the front instead of leaving REAPER stuck.
void xw_raise_modals()
{
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c) continue;
        if (c->modals.empty()) continue;
        for (auto &md : c->modals)
            if (md.gtk_win)
            {
                gtk_window_present(GTK_WINDOW(md.gtk_win));
                gtk_window_set_keep_above(GTK_WINDOW(md.gtk_win), TRUE);
            }
    }
}

// True if any captured plugin currently has an open modal.
static bool any_modal_open()
{
    for (auto &kv : g_captures)
        if (kv.second && !kv.second->modals.empty()) return true;
    return false;
}


static bool on_button_press(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;
    // A modal is open: a click on the plugin GUI must bring the modal back to the
    // front, not interact with the plugin. present()/keep_above don't work here
    // (see force_modals_top) so we remap the modal, which the compositor always
    // places on top. Don't SetFocus and don't forward the click.
    if (any_modal_open()){
        xw_raise_modals();
        return true;
    }
    if (c->hwnd) SetFocus(c->hwnd);
    // if (!gtk_widget_has_focus(widget))
    //     gtk_widget_grab_focus(widget);
    forward_motion(c, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(c->dpy, e->button, True, CurrentTime);
    XFlush(c->dpy);
    return true;
}

static bool on_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;
    forward_motion(c, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(c->dpy, e->button, False, CurrentTime);
    XFlush(c->dpy);
    return true;
}

static bool on_motion(GtkWidget *, GdkEventMotion *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;
    forward_motion(c, (int)e->x, (int)e->y);
    XFlush(c->dpy);
    return true;
}

static bool on_scroll(GtkWidget *, GdkEventScroll *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;
    unsigned int btn = 0;
    switch (e->direction) {
        case GDK_SCROLL_UP:    btn = 4; break;
        case GDK_SCROLL_DOWN:  btn = 5; break;
        case GDK_SCROLL_LEFT:  btn = 6; break;
        case GDK_SCROLL_RIGHT: btn = 7; break;
        default: return false;
    }
    forward_motion(c, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(c->dpy, btn, True,  CurrentTime);
    XTestFakeButtonEvent(c->dpy, btn, False, CurrentTime);
    XFlush(c->dpy);
    return true;
}

bool on_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{
    Capture *c = (Capture*)data;
    // DEBUG_PRINT("[GTK] enter widget=%p\n", widget);
    // Reset the cursor. Otherwise the draw area inherits the toplevel's cursor,
    // so REAPER's FX-list resize cursor (set while hovering the splitter) sticks
    // as you move into the plugin GUI. Force the default arrow on entry.
    GdkWindow *gw = gtk_widget_get_window(widget);
    if (gw) {
        GdkCursor *cur = gdk_cursor_new_from_name(gdk_window_get_display(gw), "default");
        gdk_window_set_cursor(gw, cur);
        if (cur) g_object_unref(cur);
    }
    XRaiseWindow(c->dpy, c->parent_win);
    XFlush(c->dpy);
    xw_raise_modals();
    return false;
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
        c->damage      = XDamageCreate(g_wm_dpy, plugin_win, XDamageReportBoundingBox);
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
    // Ensure any pending resize is applied server-side before we (re)name the
    // composite pixmap — otherwise we grab a pixmap that the server immediately
    // invalidates, and the next free hits BadPixmap.
    XFlush(c->dpy);
    Pixmap old = c->pixmap;
    c->pixmap = XCompositeNameWindowPixmap(c->dpy, c->plugin_win);
    if (old) XFreePixmap(c->dpy, old);
    if (c->widget) gtk_widget_queue_draw(c->widget);
}

static void cleanup_capture(Capture *c)
{
    if (!c) return;
    unregister_capture(c);
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

static bool canvas_draw_cb(GtkWidget *, cairo_t *cr, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;

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
    return true;
}

static bool canvas_button_press(GtkWidget *, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;

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
            return true;
        }
    }
    // Clicked outside any popup — dismiss.
    if (!hit) {
        if (c->popup_canvas) gtk_widget_hide(c->popup_canvas);
        XTestFakeButtonEvent(c->dpy, e->button, True,  CurrentTime);
        XFlush(c->dpy);
        XTestFakeButtonEvent(c->dpy, e->button, False, CurrentTime);
        XFlush(c->dpy);
        return true;
    }
    return false;
}

static bool canvas_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;
    XTestFakeButtonEvent(c->dpy, e->button, False, CurrentTime);
    XFlush(c->dpy);
    return true;
}

static bool canvas_motion(GtkWidget *, GdkEventMotion *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;

    static guint32 last_time = 0;
    guint32 now = g_get_monotonic_time() / 1000;
    if (now - last_time < 16) return true;
    last_time = now;

    int screen_x = (int)e->x + c->canvas_origin_x;
    int screen_y = (int)e->y + c->canvas_origin_y;
    int x11_x = screen_x - c->gtk_x;
    int x11_y = screen_y - c->gtk_y;

    // Deliver motion to the popup under the cursor with a synthetic MotionNotify
    // instead of XTestFakeMotionEvent. Warping the pointer via XTest on a nested
    // XWayland is unreliable and *blocks* — it round-trips to the parent compositor
    // (see KDE #442846, and SDL disabling XTest on XWayland) — which is the popup
    // hover hang. XSendEvent just posts the event to the popup: no warp, no
    // round-trip, nothing to block on.
    for (auto &p : c->popups) {
        if (!p.visible) continue;
        if (screen_x >= p.x && screen_x < p.x + p.w &&
            screen_y >= p.y && screen_y < p.y + p.h) {
            XEvent me; memset(&me, 0, sizeof(me));
            me.type              = MotionNotify;
            me.xmotion.display   = c->dpy;
            me.xmotion.window    = p.x11_win;
            me.xmotion.root      = DefaultRootWindow(c->dpy);
            me.xmotion.subwindow = None;
            me.xmotion.time      = CurrentTime;
            me.xmotion.x         = screen_x - p.x;   // local to the popup
            me.xmotion.y         = screen_y - p.y;
            me.xmotion.x_root    = x11_x;
            me.xmotion.y_root    = x11_y;
            me.xmotion.same_screen = True;
            XSendEvent(c->dpy, p.x11_win, True, PointerMotionMask, &me);
            XFlush(c->dpy);

            if (p.pixmap != None) XFreePixmap(c->dpy, p.pixmap);
            p.pixmap = XCompositeNameWindowPixmap(c->dpy, p.x11_win);
            if (c->canvas_draw) gtk_widget_queue_draw(c->canvas_draw);
            break;
        }
    }
    return true;
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

    // Refresh the screen offset from the plugin widget's LIVE origin instead of
    // trusting c->gtk_x/gtk_y cached by xw_size. Those go stale when a popup opens
    // before xw_size has run with correct geometry — a startup race, and always
    // after a Super+Q reopen (fresh capture) — which is what puts popups at the
    // wrong place. gdk_window_get_origin is always current; gtk_x = origin - px
    // keeps the existing (px + attr + gtk_x) formula and the motion mapping correct.
    if (c->widget) {
        GdkWindow *ww = gtk_widget_get_window(c->widget);
        if (ww) {
            int wox = 0, woy = 0;
            gdk_window_get_origin(ww, &wox, &woy);
            c->gtk_x = wox - px;
            c->gtk_y = woy - py;
        }
    }

    if (!c->popup_canvas) create_popup_canvas(c);

    XCompositeRedirectWindow(c->dpy, x11_win, CompositeRedirectAutomatic);
    XFlush(c->dpy);   // non-blocking; don't stall the main loop on every popup
    Pixmap pixmap = XCompositeNameWindowPixmap(c->dpy, x11_win);

    // Update-or-insert: if this window already has an entry, reuse it and free the
    // stale pixmap (Wine reuses window IDs; a duplicate would leave a freed pixmap
    // being composited). One entry per window.
    Capture::PopupWin *pp = nullptr;
    for (auto &e : c->popups) if (e.x11_win == x11_win) { pp = &e; break; }
    if (pp) {
        if (pp->pixmap != None && pp->pixmap != pixmap) XFreePixmap(c->dpy, pp->pixmap);
    } else {
        c->popups.emplace_back();
        pp = &c->popups.back();
    }
    pp->x11_win = x11_win;
    pp->pixmap  = pixmap;
    pp->x = px + attr->x + c->gtk_x;
    pp->y = py + attr->y + c->gtk_y;
    pp->w = attr->width;
    pp->h = attr->height;
    pp->visible = true;

    if (c->root_popup == None) c->root_popup = x11_win;

    if (!gtk_widget_get_visible(c->popup_canvas))
        gtk_widget_show_all(c->popup_canvas);
    if (c->canvas_draw) gtk_widget_queue_draw(c->canvas_draw);
}

static void canvas_remove_popup(Capture *c, Window x11_win)
{
    if (!c) return;
    for (auto it = c->popups.begin(); it != c->popups.end(); ) {
        if (it->x11_win == x11_win) {
            if (it->pixmap != None) { XFreePixmap(c->dpy, it->pixmap); it->pixmap = None; }
            it = c->popups.erase(it);
        } else ++it;
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

struct ModalRender { Capture *cap; Window x11_win; };

static bool modal_draw_cb(GtkWidget *, cairo_t *cr, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap || !m->cap->dpy) return false;
    Display *dpy = m->cap->dpy;
    Pixmap pm = 0;
    for (auto &md : m->cap->modals)
        if (md.x11_win == m->x11_win) { pm = md.pixmap; break; }
    if (!pm) return false;

    Window rr; int gx, gy; unsigned int gw, gh, gb, gd;
    if (!XGetGeometry(dpy, pm, &rr, &gx, &gy, &gw, &gh, &gb, &gd)) return FALSE;

    XWindowAttributes wa;
    Visual *visual = DefaultVisual(dpy, DefaultScreen(dpy));
    if (XGetWindowAttributes(dpy, m->x11_win, &wa)) visual = wa.visual;

    cairo_surface_t *surf = cairo_xlib_surface_create(dpy, pm, visual, gw, gh);
    if (surf) {
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        cairo_surface_destroy(surf);
    }
    return true;
}

static void modal_forward_motion(ModalRender *m, int wx, int wy)
{
    Window child; int rx, ry;
    XTranslateCoordinates(m->cap->dpy, m->x11_win, DefaultRootWindow(m->cap->dpy),
                          wx, wy, &rx, &ry, &child);
    XTestFakeMotionEvent(m->cap->dpy, DefaultScreen(m->cap->dpy), rx, ry, CurrentTime);
}

static bool modal_button_press(GtkWidget *, GdkEventButton *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->dpy) return false;
    modal_forward_motion(m, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(m->cap->dpy, e->button, True, CurrentTime);
    XFlush(m->cap->dpy);
    return true;
}

static bool modal_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->dpy) return false;
    modal_forward_motion(m, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(m->cap->dpy, e->button, False, CurrentTime);
    XFlush(m->cap->dpy);
    return true;
}

static bool modal_motion(GtkWidget *, GdkEventMotion *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->dpy) return false;
    modal_forward_motion(m, (int)e->x, (int)e->y);
    XFlush(m->cap->dpy);
    return true;
}

static bool modal_key(GtkWidget *, GdkEventKey *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->dpy) return false;

    // Modals are separate top-level GTK windows, so they bypass the bridge's
    // OnKeyEvent/xw_forward_key path. Forward keys straight to the modal's own
    // X11 window on :10, or it never gets keyboard input (Enter/Esc/text) and wedges.
    XKeyEvent xev; memset(&xev, 0, sizeof(xev));
    xev.type        = (e->type == GDK_KEY_PRESS) ? KeyPress : KeyRelease;
    xev.display     = m->cap->dpy;
    xev.window      = m->x11_win;
    xev.root        = DefaultRootWindow(m->cap->dpy);
    xev.time        = CurrentTime;
    xev.keycode     = e->hardware_keycode;
    xev.state       = e->state;
    xev.same_screen = True;
    XSendEvent(m->cap->dpy, m->x11_win, True,
               (e->type == GDK_KEY_PRESS) ? KeyPressMask : KeyReleaseMask,
               (XEvent*)&xev);
    XFlush(m->cap->dpy);
    return true;
}

static void modal_render_destroy(gpointer data, GClosure *) { delete (ModalRender*)data; }

static void create_modal(Capture *state, Window win, XWindowAttributes *attr)
{
    // Never create a second modal for the same window.
    for (auto &m : state->modals) if (m.x11_win == win) return;

    Display *dpy = state->dpy;
    XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);
    XFlush(dpy);
    Pixmap pm = XCompositeNameWindowPixmap(dpy, win);

    GtkWidget *gtk_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(gtk_win), TRUE);

    GtkWidget *toplevel = state->widget ? gtk_widget_get_toplevel(state->widget) : nullptr;
    if (toplevel && GTK_IS_WINDOW(toplevel))
        gtk_window_set_transient_for(GTK_WINDOW(gtk_win), GTK_WINDOW(toplevel));

    gtk_window_set_type_hint(GTK_WINDOW(gtk_win), GDK_WINDOW_TYPE_HINT_DIALOG);

    gtk_window_resize(GTK_WINDOW(gtk_win), attr->width, attr->height);

    GtkWidget *draw = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw, attr->width, attr->height);
    gtk_container_add(GTK_CONTAINER(gtk_win), draw);

    ModalRender *mr = new ModalRender();
    mr->cap = state; mr->x11_win = win;

    gtk_widget_add_events(draw, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    gtk_widget_set_can_focus(draw, TRUE);
    g_signal_connect(draw, "button-press-event",   G_CALLBACK(modal_button_press),   mr);
    g_signal_connect(draw, "button-release-event", G_CALLBACK(modal_button_release), mr);
    g_signal_connect(draw, "motion-notify-event",  G_CALLBACK(modal_motion),         mr);
    g_signal_connect(draw, "key-press-event",      G_CALLBACK(modal_key),            mr);
    g_signal_connect(draw, "key-release-event",    G_CALLBACK(modal_key),            mr);
    g_signal_connect_data(draw, "draw", G_CALLBACK(modal_draw_cb), mr, modal_render_destroy, (GConnectFlags)0);

    gtk_widget_show_all(gtk_win);
    gtk_widget_grab_focus(draw);

    Damage dmg = 0;
    if (g_wm_dpy && g_damage_event_base >= 0)
        dmg = XDamageCreate(g_wm_dpy, win, XDamageReportBoundingBox);

    Capture::ModalWin md;
    md.x11_win = win; md.pixmap = pm; md.gtk_win = gtk_win; md.draw = draw; md.damage = dmg;
    state->modals.push_back(md);
}

static void modal_remove(Capture *c, Window win)
{
    for (auto it = c->modals.begin(); it != c->modals.end(); ++it)
        if (it->x11_win == win) {
            // Pull resources out and drop the entry BEFORE destroying the widget:
            // gtk_widget_destroy can dispatch events (re-entrancy), and on Super+Q
            // GTK may already have destroyed the modal — so guard with GTK_IS_WIDGET
            // to avoid the 'GTK_IS_WIDGET (widget)' assertion on a dead widget.
            Damage     dmg = it->damage;
            Pixmap     pm  = it->pixmap;
            GtkWidget *w   = it->gtk_win;
            c->modals.erase(it);
            if (dmg && g_wm_dpy)       XDamageDestroy(g_wm_dpy, dmg);
            if (pm)                    XFreePixmap(c->dpy, pm);
            if (w && GTK_IS_WIDGET(w)) gtk_widget_destroy(w);
            break;
        }
}

static void handle_new_window(Window win, Capture *state, XWindowAttributes *attr) {
    if (!state->dpy || !win || !state) return;

    // Already tracked? A repeated MapNotify (Wine re-maps) must not spawn a second
    // popup/modal. Refresh an existing popup; leave an existing modal alone.
    for (auto &p : state->popups)
        if (p.x11_win == win) { canvas_add_popup(state, win, attr); return; }
    for (auto &m : state->modals)
        if (m.x11_win == win) return;

    bool is_popup = classify_popup(state->dpy, win, attr);
    if (is_popup) {
        if ((attr->width <= 1 || attr->height <= 1)) {
            DEBUG_PRINT("Skipping 1x1 window: 0x%lx (%dx%d)\n", win, attr->width, attr->height);
            return;
        }

        if ((attr->width == 12 || attr->height == 12)) {
            DEBUG_PRINT("Skipping unnamed shadow JUCE window: 0x%lx (%dx%d)\n", win, attr->width, attr->height);
            return;
        }
        canvas_add_popup(state, win, attr);
    } else {
        if (attr->width <= 1 || attr->height <= 1) return;
        create_modal(state, win, attr);
    }
}

static void on_popup_mapped(Window w)
{
    XWindowAttributes attr;
    if (!XGetWindowAttributes(g_wm_dpy, w, &attr) || attr.map_state != IsViewable)
        return;

    for (auto &kv : g_captures) {
        Capture *cand = kv.second;
        if (cand && is_window_from_owned_plugin(cand->dpy, w, cand->gui_win)) {
            handle_new_window(w, cand, &attr);
            return;
        }
    }
}

static void on_popup_unmapped(Window w)
{
    fprintf(stderr, "[xwb] popup_unmapped w=0x%lx\n", w); fflush(stderr);
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        auto it = std::find_if(c->popups.begin(), c->popups.end(),
            [w](const Capture::PopupWin &p){ return p.x11_win == w; });
        if (it != c->popups.end()) { canvas_remove_popup(c, w); return; }

        auto mit = std::find_if(c->modals.begin(), c->modals.end(),
            [w](const Capture::ModalWin &m){ return m.x11_win == w; });
        if (mit != c->modals.end()) { modal_remove(c, w); return; }
    }
}

// A popup moved/resized (drag-and-drop popup). Update its geometry from the raw
// configure coords (already root-relative on :10) and re-capture its pixmap.
static bool on_popup_configured(Window w, int cx, int cy, int cw, int ch)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        for (auto &p : c->popups) {
            if (p.x11_win != w) continue;
            p.x = cx; p.y = cy; p.w = cw; p.h = ch;
            if (p.pixmap != None) XFreePixmap(c->dpy, p.pixmap);
            p.pixmap = XCompositeNameWindowPixmap(c->dpy, w);
            if (c->canvas_draw) {
                GdkWindow *cwin = gtk_widget_get_window(c->canvas_draw);
                if (cwin) gdk_window_invalidate_rect(cwin, NULL, FALSE);
                else      gtk_widget_queue_draw(c->canvas_draw);
            }
            return true;
        }
    }
    return false;
}

// Bridge per-event handling, registered as g_wm->on_unhandled_event. The WM has
// already processed the event (MapRequest/ConfigureRequest/etc); here we deal
// only with our own concerns: popup/modal lifecycle.
static void bridge_handle_event(XEvent *ev)
{
    // Damage may target a plugin GUI or a modal dialog — resolve by the damaged
    // drawable (XDamageNotifyEvent.drawable overlaps xany.window). Modals aren't
    // in g_captures, so we test the event type first, then look up the drawable.
    if (g_damage_event_base >= 0 && ev->type == g_damage_event_base + XDamageNotify) {
        XDamageNotifyEvent *de = (XDamageNotifyEvent *)ev;
        DEBUG_PRINT("damage: %dx%d+%d+%d\n",
            de->area.width, de->area.height, de->area.x, de->area.y);
        // Subtract on g_wm_dpy — the connection that created the damage object and
        // receives its events — so the region actually clears and re-arms.
        XDamageSubtract(g_wm_dpy, de->damage, None, None);

        // Plugin GUI: de->area is in plugin_win coords, which map 1:1 to the
        // widget (on_draw blits the pixmap at 0,0). cairo clips the paint to this
        // rect, so only this region blits. Modals work the same way against md.draw.
        Capture *c = find_capture(de->drawable);
        if (c && c->widget && GTK_IS_WIDGET(c->widget)) {
            gtk_widget_queue_draw_area(c->widget, de->area.x, de->area.y,
                                       de->area.width, de->area.height);
            return;
        }
        for (auto &kv : g_captures) {
            Capture *cc = kv.second;
            if (!cc) continue;
            for (auto &md : cc->modals) {
                if (md.x11_win == de->drawable) {
                    if (md.draw && GTK_IS_WIDGET(md.draw))
                        gtk_widget_queue_draw_area(md.draw, de->area.x, de->area.y,
                                                   de->area.width, de->area.height);
                    return;
                }
            }
        }
        return;
    }

    // Non-damage: popup lifecycle. (c set => plugin window, nothing to do here.)
    Capture *c = find_capture(ev->xany.window);
    if (c) return;
    switch (ev->type)
    {
        case MapNotify:
            on_popup_mapped(ev->xmap.window);
            break;
        case UnmapNotify:
            on_popup_unmapped(ev->xunmap.window);
            break;
        case ConfigureNotify:
            on_popup_configured(ev->xconfigure.window,
                                ev->xconfigure.x, ev->xconfigure.y,
                                ev->xconfigure.width, ev->xconfigure.height);
            break;
        default:
            break;
    }
}

// True if any captured plugin currently has an open popup but NO open modal.
// Used to scope the Escape-on-click popup-dismiss workaround so it never fires
// while a modal is up (which would wrongly close the modal).
void xw_bridge_swell_on_button_event_escape()
{
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c) continue;
        if (!c->modals.empty())
            xw_raise_modals();
        if (!c->popups.empty()){
            KeyCode esc = XKeysymToKeycode(g_wm_dpy, XK_Escape);
            XTestFakeKeyEvent(g_wm_dpy, esc, True, CurrentTime);
            XTestFakeKeyEvent(g_wm_dpy, esc, False, CurrentTime);
            XFlush(g_wm_dpy);
        }
    }
}

// If any plugin popup is open, dismiss it and return true. Called from SWELL's
// GDK_DELETE handler when a window is being closed (e.g. Hyprland Super+Q).
//
// The overlay canvas is a GTK_WINDOW_POPUP made transient to the plugin's FX
// window, i.e. an xdg_popup child on Wayland. If that popup is still mapped when
// its parent (the FX window) closes, the compositor raises an xdg_shell protocol
// error that kills GTK's Wayland connection and hangs REAPER instantly. So we
// must fully DESTROY the canvas here (gtk_widget_destroy, not hide) to tear down
// the xdg_popup and its grab before any close proceeds. We also Escape the :10
// menu so the plugin drops its own X grab.
bool xw_bridge_swell_on_gdk_delete_release()
{
    bool any = false;
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (!c) continue;
        if (!c->popups.empty()) {
            any = true;
            for (auto &p : c->popups)
                if (p.pixmap != None && c->dpy) XFreePixmap(c->dpy, p.pixmap);
            c->popups.clear();
            c->root_popup = None;
            if (c->popup_canvas && GTK_IS_WIDGET(c->popup_canvas)) {
                gtk_widget_destroy(c->popup_canvas);
                c->popup_canvas = nullptr;
                c->canvas_draw  = nullptr;
            }
        }
        if (!c->modals.empty()) {
            // any = true;   // a modal also needs the Escapefor (auto &md : c->modals) {
            for (auto &md : c->modals) {
                if (md.gtk_win &&
                    gtk_window_is_active(GTK_WINDOW(md.gtk_win))) {
                    any = true;
                    break;
                }
            }
        }
    }
    // Only touch :10 / send Escape when there was actually a popup or modal to
    // dismiss. Firing Escape on EVERY window close — including a plain plugin close
    // with nothing open — is what corrupts the teardown and crashes Wine (the
    // plugin's GUI window gets destroyed from the outside -> BadWindow).
    if (any) {
        gdk_display_flush(gdk_display_get_default());
        if (g_wm_dpy) {
            KeyCode esc = XKeysymToKeycode(g_wm_dpy, XK_Escape);
            XTestFakeKeyEvent(g_wm_dpy, esc, True,  CurrentTime);
            XTestFakeKeyEvent(g_wm_dpy, esc, False, CurrentTime);
            XFlush(g_wm_dpy);
        }
    }
    return any;
}

void init_private_xwayland()
{
    // The WM owns Xwayland spawn, the X connection, error handling.
    // We just bring it up and register our per-event handler.
    if (!XWaylandWM::init_bridge_wm(":10")) return;

    if (g_wm_dpy) {
        int base, err;
        if (XDamageQueryExtension(g_wm_dpy, &base, &err)) {
            g_damage_event_base = base;
            // Expose the Damage error base so the WM's X error handler can swallow
            // BadDamage. modal_remove runs on UnmapNotify, but a closing dialog is
            // usually destroyed right after, and the server auto-frees its damage
            // on DestroyNotify — so our XDamageDestroy races an already-freed damage.
            g_bridge_damage_error_base = err;
        }
    }

    g_wm->on_unhandled_event = bridge_handle_event;
}

static bool try_create_plugin(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return true;
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

    return true;
}

void xw_destroy(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return;
    bridgeState *bs = (bridgeState*)hwnd->m_private_data;
    // m_oswidget is non-NULL here only if the widget is still alive (the weak
    // pointer nulls it on destroy), so this is safe.
    if (hwnd->m_oswidget && GTK_IS_WIDGET(hwnd->m_oswidget)) {
        g_object_remove_weak_pointer(G_OBJECT(hwnd->m_oswidget), (gpointer*)&hwnd->m_oswidget);
        GtkWidget *parent = gtk_widget_get_parent(hwnd->m_oswidget);
        if (parent) gtk_container_remove(GTK_CONTAINER(parent), hwnd->m_oswidget);
    }
    if (bs->cap) cleanup_capture(bs->cap);
    hwnd->m_private_data = 0;
    delete bs;
}

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
        pos_x = fp->m_position.left + r.left;
        pos_y = fp->m_position.top  + r.top;
        // PLUGIN IN FX LIST
        if (fp->m_parent) {
            pos_y += fp->m_parent->m_position.bottom - fp->m_position.bottom;
        }
    }
    int w = r.right - r.left, h = r.bottom - r.top;

    // Feed the same GTK offset to the popup canvas so popups position correctly
    // in both floating and FX-list embedded modes.
    if (bs->cap) { bs->cap->gtk_x = pos_x; bs->cap->gtk_y = pos_y; }

    // Re-capture the pixmap at the new size — xw_size runs exactly when the window
    // is being resized, so the backing pixmap must be refreshed here. Guard on
    // bs->cap: WM_SIZE/SetWindowPos can fire before the plugin is captured (e.g.
    // during an FX-list swap), and the container resize also needs a live capture.
    if (bs->cap) {
        refresh_pixmap(bs->cap);
        XResizeWindow(bs->cap->dpy, bs->cap->parent_win, w, h);
        XFlush(bs->cap->dpy);
    }
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

        // **Request parent window resize to fit new plugin size**
        if (GTK_IS_WINDOW(parent->m_oswidget)) {
            int needed_width = pos_x + (r.right - r.left);
            int needed_height = pos_y + (r.bottom - r.top);

            // Set minimum size requirement
            gtk_widget_set_size_request(GTK_WIDGET(parent->m_oswidget), 
                                        needed_width, 
                                        needed_height);

            // Trigger relayout
            gtk_widget_queue_resize(GTK_WIDGET(parent->m_oswidget));

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
        case WM_TIMER: try_create_plugin(hwnd); break;
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
    // Auto-null m_oswidget the moment the widget is destroyed (e.g. its container
    // is torn down when the plugin/FX window closes). Without this the pointer
    // dangles and later GTK calls (get_parent/container_remove during teardown or
    // reopen) hit freed memory — GTK_IS_WIDGET can't catch that. With the weak
    // pointer, m_oswidget != NULL reliably means the widget is alive.
    g_object_add_weak_pointer(G_OBJECT(draw_area), (gpointer*)&hwnd->m_oswidget);

    bridgeState *bs = new bridgeState();
    bs->disp   = disp;
    bs->parent = container;
    hwnd->m_private_data = (INT_PTR)bs;

    *wref = (void*)container;
    SetTimer(hwnd, 1, 100, NULL);
    return hwnd;
}

// Called from SWELL OnKeyEvent
bool xw_bridge_forward_key(HWND hwnd, int keycode, int state, bool is_press)
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
