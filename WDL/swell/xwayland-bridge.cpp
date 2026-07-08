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

    // ── Modals (real dialog windows the plugin opens) ──
    struct ModalWin { Window x11_win; Pixmap pixmap; GtkWidget *gtk_win; GtkWidget *draw; };
    std::vector<ModalWin> modals;
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
    if (c->damage) XDamageDestroy(c->dpy, c->damage);
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

static bool is_window_from_owned_plugin(Display *dpy, Window win, Capture *state) {
    // Check if this window has the same PID as our plugin
    Atom atom_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    pid_t window_pid = 0;
    if (XGetWindowProperty(dpy, win, atom_pid, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success) {
        if (prop && nitems > 0) {
            window_pid = *((pid_t*)prop);
            XFree(prop);
        }
    }

    // Get our plugin's PID (from main_plugin_gui)
    pid_t our_pid = 0;
    if (XGetWindowProperty(dpy, state->gui_win, atom_pid, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success) {
        if (prop && nitems > 0) {
            our_pid = *((pid_t*)prop);
            XFree(prop);
        }
    }

    if (window_pid == 0 || our_pid == 0 || window_pid != our_pid) {
        return FALSE;
    }

    return TRUE;
}

// Motif hints tell us whether a window wants decorations. yabridge popups and
// modals both arrive as NORMAL type with the yabridge class; the difference is
// that a popup disables decorations (MWM_HINTS_DECORATIONS set, decorations==0)
// while a real modal dialog keeps them. This is the signal that separates them.
struct MotifWmHints {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long          input_mode;
    unsigned long status;
};
#define MWM_HINTS_DECORATIONS (1L << 1)

static bool hasMotifHints(Display *dpy, Window win, MotifWmHints &out) {
    Atom a = XInternAtom(dpy, "_MOTIF_WM_HINTS", True);
    if (a == None) return false;

    Atom actual_type; int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = nullptr;
    if (XGetWindowProperty(dpy, win, a, 0, 5, False, a,
                           &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) != Success)
        return false;
    if (!prop || actual_format != 32 || nitems < 5) { if (prop) XFree(prop); return false; }

    const long *l = reinterpret_cast<long*>(prop);
    out.flags = l[0]; out.functions = l[1]; out.decorations = l[2];
    out.input_mode = l[3]; out.status = l[4];
    XFree(prop);
    return true;
}

static bool classify_popup(Display *dpy, Window win, XWindowAttributes *attr) {
    if (!dpy || !win) return false;

    Atom atom_window_type        = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom atom_type_normal        = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    Atom atom_type_dialog        = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    Atom atom_type_popup_menu    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    Atom atom_type_menu          = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    Atom atom_type_dropdown_menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    Atom atom_type_tooltip       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    Atom atom_type_dnd           = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DND", False);
    Atom atom_type_utility       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);

    bool is_popup = false;
    Window transient_for = None;
    XGetTransientForHint(dpy, win, &transient_for);

    std::vector<Atom> window_types;
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char* prop = nullptr;

    if (XGetWindowProperty(dpy, win, atom_window_type, 0, 10,
                           False, XA_ATOM, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success) {
        if (prop) {
            if (actual_format == 32 && nitems > 0) {
                Atom* atoms = (Atom*)prop;  // Simple C cast is fine
                window_types.assign(atoms, atoms + nitems);
            }
            XFree(prop);
        }
    }

    if (window_types.empty()) {
        if (transient_for != None) {
            window_types.push_back(atom_type_dialog);
        } else {
            window_types.push_back(atom_type_normal);
        }
    }

    bool override_redirect = attr->override_redirect;

    // Decoration state: a decoration-less window is a popup, a decorated one is
    // a real (modal) window
    bool motif_popup = false;
    MotifWmHints mh;
    if (hasMotifHints(dpy, win, mh))
        motif_popup = (mh.flags & MWM_HINTS_DECORATIONS) && mh.decorations == 0;

    for (Atom ty : window_types) {
        if (ty == atom_type_normal) {
            is_popup = override_redirect || motif_popup;
        }
        else if (ty == atom_type_dialog || ty == atom_type_utility) {
            is_popup = override_redirect;
        }
        else if (
            ty == atom_type_popup_menu ||
            ty == atom_type_menu ||
            ty == atom_type_dropdown_menu ||
            ty == atom_type_tooltip ||
            ty == atom_type_dnd
        ) {
            is_popup = true;
        }
        else {
            continue;   // unknown → try next
        }
        break;
    }
    return is_popup;
}

struct ModalRender { Capture *cap; Window x11_win; };

static gboolean modal_draw_cb(GtkWidget *, cairo_t *cr, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap || !m->cap->dpy) return FALSE;
    Display *dpy = m->cap->dpy;
    Pixmap pm = 0;
    for (auto &md : m->cap->modals)
        if (md.x11_win == m->x11_win) { pm = md.pixmap; break; }
    if (!pm) return FALSE;

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
    return TRUE;
}

static void modal_forward_motion(ModalRender *m, int wx, int wy)
{
    Window child; int rx, ry;
    XTranslateCoordinates(m->cap->dpy, m->x11_win, DefaultRootWindow(m->cap->dpy),
                          wx, wy, &rx, &ry, &child);
    XTestFakeMotionEvent(m->cap->dpy, DefaultScreen(m->cap->dpy), rx, ry, CurrentTime);
}

static gboolean modal_button_press(GtkWidget *, GdkEventButton *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->dpy) return FALSE;
    modal_forward_motion(m, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(m->cap->dpy, e->button, True, CurrentTime);
    XFlush(m->cap->dpy);
    return TRUE;
}

static gboolean modal_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->dpy) return FALSE;
    modal_forward_motion(m, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(m->cap->dpy, e->button, False, CurrentTime);
    XFlush(m->cap->dpy);
    return TRUE;
}

static gboolean modal_motion(GtkWidget *, GdkEventMotion *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->dpy) return FALSE;
    modal_forward_motion(m, (int)e->x, (int)e->y);
    XFlush(m->cap->dpy);
    return TRUE;
}

static void modal_render_destroy(gpointer data, GClosure *) { delete (ModalRender*)data; }

static void create_modal(Capture *state, Window win, XWindowAttributes *attr)
{
    // Never create a second modal for the same window.
    for (auto &m : state->modals) if (m.x11_win == win) return;

    Display *dpy = state->dpy;
    XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);
    XSync(dpy, False);
    Pixmap pm = XCompositeNameWindowPixmap(dpy, win);

    GtkWidget *gtk_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(gtk_win), TRUE);

    GtkWidget *toplevel = state->widget ? gtk_widget_get_toplevel(state->widget) : nullptr;
    if (toplevel && GTK_IS_WINDOW(toplevel))
        gtk_window_set_transient_for(GTK_WINDOW(gtk_win), GTK_WINDOW(toplevel));

    gtk_window_resize(GTK_WINDOW(gtk_win), attr->width, attr->height);

    GtkWidget *draw = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw, attr->width, attr->height);
    gtk_container_add(GTK_CONTAINER(gtk_win), draw);

    ModalRender *mr = new ModalRender();
    mr->cap = state; mr->x11_win = win;

    gtk_widget_add_events(draw, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(draw, "button-press-event",   G_CALLBACK(modal_button_press),   mr);
    g_signal_connect(draw, "button-release-event", G_CALLBACK(modal_button_release), mr);
    g_signal_connect(draw, "motion-notify-event",  G_CALLBACK(modal_motion),         mr);
    g_signal_connect_data(draw, "draw", G_CALLBACK(modal_draw_cb), mr, modal_render_destroy, (GConnectFlags)0);

    gtk_widget_show_all(gtk_win);

    Capture::ModalWin md;
    md.x11_win = win; md.pixmap = pm; md.gtk_win = gtk_win; md.draw = draw;
    state->modals.push_back(md);
}

static void modal_refresh(Capture *c, Window win)
{
    for (auto &md : c->modals)
        if (md.x11_win == win) {
            if (md.pixmap) XFreePixmap(c->dpy, md.pixmap);
            md.pixmap = XCompositeNameWindowPixmap(c->dpy, win);
            if (md.draw) gtk_widget_queue_draw(md.draw);
            break;
        }
}

static void modal_remove(Capture *c, Window win)
{
    for (auto it = c->modals.begin(); it != c->modals.end(); ++it)
        if (it->x11_win == win) {
            if (it->pixmap) XFreePixmap(c->dpy, it->pixmap);
            if (it->gtk_win) gtk_widget_destroy(it->gtk_win);
            c->modals.erase(it);
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
// ─── :10 event loop (on the GLib main loop, main thread) ─────────────────────

// A newly-mapped override-redirect window is a plugin popup. Attribute it to the
// capture whose plugin shares its PID (correct with multiple plugins open), then
// hand it to that capture's popup canvas.
static void on_popup_mapped(Window w)
{
    XWindowAttributes attr;
    if (!XGetWindowAttributes(g_wm_dpy, w, &attr) || attr.map_state != IsViewable)
        return;

    for (auto &kv : g_captures) {
        Capture *cand = kv.second;
        if (cand && is_window_from_owned_plugin(cand->dpy, w, cand)) {
            handle_new_window(w, cand, &attr);
            return;
        }
    }
}

// A popup was unmapped — remove it from whichever capture owns it.
static void on_popup_unmapped(Window w)
{
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

static bool refresh_modal_if_any(Window w)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        for (auto &md : c->modals)
            if (md.x11_win == w) { modal_refresh(c, w); return true; }
    }
    return false;
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

static gboolean wm_event_cb(GIOChannel *, GIOCondition, gpointer)
{
    while (XPending(g_wm_dpy))
    {
        XEvent ev;
        XNextEvent(g_wm_dpy, &ev);

        // Let the ICCCM WM layer handle MapRequest/ConfigureRequest/etc first.
        if (g_wm) g_wm->handle_event(&ev);

        Capture *c = find_capture(ev.xany.window);

        // Modals aren't registered captures; refresh them on their own events.
        if (!c && (ev.type == ConfigureNotify || ev.type == Expose || ev.type == MapNotify)) {
            if (refresh_modal_if_any(ev.xany.window)) continue;
        }

        // Damage on a known capture → re-grab the pixmap and redraw.
        if (c && c->damage_base && ev.type == c->damage_base + XDamageNotify) {
            XDamageSubtract(c->dpy, c->damage, None, None);
            refresh_pixmap(c);
            continue;
        }

        switch (ev.type)
        {
            case MapNotify:
                if (c) {
                    // One of a capture's own windows mapped.
                    if (ev.xmap.window == c->plugin_win || ev.xmap.window == c->gui_win) {
                        XSync(c->dpy, False);
                        refresh_pixmap(c);
                    }
                } else {
                    // Not a known capture window → candidate plugin popup.
                    on_popup_mapped(ev.xmap.window);
                }
                break;

            case UnmapNotify:
                on_popup_unmapped(ev.xunmap.window);
                break;

            case ConfigureNotify:
                if (c && (ev.xconfigure.window == c->plugin_win ||
                          ev.xconfigure.window == c->gui_win)) {
                    XResizeWindow(c->dpy, c->parent_win,
                                  ev.xconfigure.width, ev.xconfigure.height);
                    XFlush(c->dpy);
                    refresh_pixmap(c);
                    if (c->hwnd) SendMessage(c->hwnd, WM_SIZE, SIZE_RESTORED, 0);
                }
                else if (!c) {
                    on_popup_configured(ev.xconfigure.window,
                                        ev.xconfigure.x, ev.xconfigure.y,
                                        ev.xconfigure.width, ev.xconfigure.height);
                }
                break;

            case Expose:
                if (c) refresh_pixmap(c);
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
