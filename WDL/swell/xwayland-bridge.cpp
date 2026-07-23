#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif
#include "xwayland-bridge.h"
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

XWaylandWM     *g_wm           = nullptr;
Display *g_wm_dpy       = nullptr;

// Layout slots on the virtual framebuffer (defined near xw_bridge_create).
static int  xw_alloc_slot(int *sx, int *sy);
static void xw_free_slot(int slot);
static bool point_in_slot(int slot, int x, int y);
static void slot_rect(int slot, int *sx, int *sy, int *sw, int *sh);

// ─── State ───────────────────────────────────────────────────────────────────
// One plugin = one capture. We composite-capture the plugin's X11 window into a
// pixmap and blit it into the SWELL GtkWidget (draw area). Input is forwarded
// back to the plugin via XTest.

struct Capture {
    Display   *dpy         = nullptr;   // per-plugin connection to :10
    Window     parent_win  = 0;         // container we created on :10
    Window     plugin_win  = 0;         // the plugin's X11 window (child of parent)
    Window     gui_win     = 0;         // Wine child GUI (or == plugin_win for native)
    Pixmap     pixmap      = 0;         // (composite pixmap: popups only now)
    // Shared-memory capture of this plugin's slot in the Xvfb framebuffer. Replaces
    // XCompositeRedirectWindow/NameWindowPixmap: the redirect blocked pointer input,
    // and on a virtual framebuffer it is unnecessary -- nothing is ever occluded or
    // off-screen, so the window's pixels can be read straight out of the root.
    XImage         *shm_img      = nullptr;
    XShmSegmentInfo shm_info     = {};
    bool            shm_attached = false;
    int             shm_w        = 0;
    int             shm_h        = 0;
    GtkWidget *widget      = nullptr;   // SWELL draw area we blit into
    HWND       hwnd        = nullptr;   // back-reference
    bool       has_painted = false;     // true once a real DamageNotify has ever been received for this capture -- see on_draw and the damage handler
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
    int        slot            = -1;    // layout slot on :10 (mirrors bridgeState::slot),
                                          // used to attribute a popup to its owning
                                          // instance when multiple share one Wine PID
    struct PopupWin { Window x11_win; Pixmap pixmap; int x,y,w,h; bool visible; Damage damage; Visual *visual; };
    std::vector<PopupWin> popups;

    // ── Modals (real dialog windows the plugin opens) ──
    struct ModalWin { Window x11_win; Pixmap pixmap; GtkWidget *gtk_win; GtkWidget *draw; Damage damage; int w; int h; Visual *visual; };
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
    int      slot   = -1;        // layout slot on :10, released on teardown
};

static void destroy_shm(Capture *c)
{
    if (c->shm_img) {
        if (c->shm_attached && c->dpy) XShmDetach(c->dpy, &c->shm_info);
        if (c->shm_info.shmaddr) shmdt(c->shm_info.shmaddr);
        c->shm_img->data = nullptr;     // shm memory, not malloc'd -- don't let Xlib free it
        XDestroyImage(c->shm_img);
        c->shm_img = nullptr;
    }
    c->shm_info.shmaddr = nullptr;
    c->shm_info.shmid   = -1;
    c->shm_attached = false;
    c->shm_w = c->shm_h = 0;
}

// (Re)allocate the shared-memory image when the plugin's size changes.
static bool ensure_shm(Capture *c, int w, int h)
{
    if (c->shm_img && c->shm_w == w && c->shm_h == h) return true;
    destroy_shm(c);
    if (w <= 0 || h <= 0) return false;

    const int screen = DefaultScreen(c->dpy);
    Visual *vis      = DefaultVisual(c->dpy, screen);
    const int depth  = DefaultDepth(c->dpy, screen);

    c->shm_img = XShmCreateImage(c->dpy, vis, depth, ZPixmap, nullptr, &c->shm_info, w, h);
    if (!c->shm_img) return false;

    c->shm_info.shmid = shmget(IPC_PRIVATE,
                               (size_t)c->shm_img->bytes_per_line * c->shm_img->height,
                               IPC_CREAT | 0600);
    if (c->shm_info.shmid < 0) { XDestroyImage(c->shm_img); c->shm_img = nullptr; return false; }

    c->shm_info.shmaddr  = c->shm_img->data = (char*)shmat(c->shm_info.shmid, nullptr, 0);
    c->shm_info.readOnly = False;
    if (c->shm_info.shmaddr == (char*)-1) {
        shmctl(c->shm_info.shmid, IPC_RMID, nullptr);
        c->shm_info.shmaddr = nullptr;
        c->shm_img->data = nullptr;
        XDestroyImage(c->shm_img); c->shm_img = nullptr;
        return false;
    }
    if (!XShmAttach(c->dpy, &c->shm_info)) { destroy_shm(c); return false; }
    XSync(c->dpy, False);
    // Mark for destruction now: it stays alive until everyone detaches, so this just
    // guarantees it cannot leak if we crash.
    shmctl(c->shm_info.shmid, IPC_RMID, nullptr);
    c->shm_info.shmid = -1;

    c->shm_attached = true;
    c->shm_w = w;
    c->shm_h = h;
    return true;
}


static bool on_draw(GtkWidget *, cairo_t *cr, gpointer data)
{
    Capture *c = (Capture*)data;
    // No XGetWindowAttributes, No ensure_shm, No XShmGetImage here!
    if (!c || !c->shm_img || !c->shm_img->data) return FALSE;

    cairo_surface_t *surf =
        cairo_image_surface_create_for_data((unsigned char*)c->shm_img->data,
                                            CAIRO_FORMAT_RGB24,
                                            c->shm_img->width, c->shm_img->height,
                                            c->shm_img->bytes_per_line);
    if (surf) {
        if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
            cairo_set_source_surface(cr, surf, 0, 0);
            cairo_paint(cr);
        }
        cairo_surface_destroy(surf);
    }

    // Some plugins still haven't painted anything into the SHM buffer by the time
    // we draw it (Xvfb is headless -- see the nudge in try_create_plugin), showing
    // up as a blank frame instead of the real UI. has_painted is a genuine signal
    // (a real DamageNotify has been received at least once), not a guess from pixel
    // color, so this nudges only while Wine truly hasn't painted anything yet, and
    // never again once it has -- a plugin whose legitimate first frame happens to be
    // dark-themed is never mistaken for still-loading.
    if (!c->has_painted && c->dpy && c->plugin_win) {
        XClearArea(c->dpy, c->plugin_win, 0, 0, 0, 0, True);
        if (c->gui_win != c->plugin_win)
            XClearArea(c->dpy, c->gui_win, 0, 0, 0, 0, True);
        XFlush(c->dpy);
    }

    return TRUE;
}


static void forward_motion(Capture *c, int wx, int wy)
{
    Window child; int rx, ry;
    XTranslateCoordinates(c->dpy, c->plugin_win, DefaultRootWindow(c->dpy),
                          wx, wy, &rx, &ry, &child);
    XTestFakeMotionEvent(c->dpy, DefaultScreen(c->dpy), rx, ry, CurrentTime);
}

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

static bool any_modal_open()
{
    for (auto &kv : g_captures)
        if (kv.second && !kv.second->modals.empty()) return true;
    return false;
}

static bool on_button_press(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    DEBUG_PRINT("[DNDX] widget button PRESS btn=%d\n", e->button);
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;
    if (any_modal_open()){
        xw_raise_modals();
        return true;
    }
    if (c->hwnd) SetFocus(c->hwnd);
    forward_motion(c, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(c->dpy, e->button, True, CurrentTime);
    XFlush(c->dpy);
    return true;
}

static bool on_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    DEBUG_PRINT("[DNDX] widget button RELEASE btn=%d\n", e->button);
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;
    forward_motion(c, (int)e->x, (int)e->y);
    XTestFakeButtonEvent(c->dpy, e->button, False, CurrentTime);
    XFlush(c->dpy);
    return true;
}

static bool on_motion(GtkWidget *, GdkEventMotion *e, gpointer data)
{
    // A plugin drag-out is in flight and the WM's XDND catcher has its file. Start the
    // native drag here, inside a real GTK input handler, while the button is still
    // held -- this is the only context where gtk_drag_begin gets the input serial
    // Wayland requires. SWELL_InitiateDragDropOfFileList drives the Wayland drag
    // source itself (dropSourceWndProc's SWELL_TARGET_WAYLAND branch) and blocks until
    // the drag ends; it pumps the message loop while it spins, so X events keep
    // flowing and yabridge keeps getting its XdndStatus replies.
    if (g_wm && g_wm->dnd_has_pending()) {
        static char path[8192];
        g_wm->dnd_take_pending_path(path, sizeof(path));
        if (path[0]) {
            DEBUG_PRINT("[DNDX] starting native drag: %s\n", path);
            Capture *cc = (Capture*)data;
            const char *lst[1] = { path };
            SWELL_InitiateDragDropOfFileList(cc ? cc->hwnd : NULL, NULL, lst, 1, NULL);
            DEBUG_PRINT("[DNDX] native drag finished\n");

            // SWELL's drag source took the capture, so the button release went to it
            // and never reached this widget -- which means on_button_release never ran
            // and :10 never saw a ButtonRelease. Release it via the plugin's own :10
            // connection or yabridge's XDND loop is left waiting for one forever.
            if (cc && cc->dpy) g_wm->dnd_release_source_button(cc->dpy);
        }
    }

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

    XSetWindowAttributes swa;
    swa.backing_store = Always;
    XChangeWindowAttributes(dpy, plugin_win, CWBackingStore, &swa);

    int base, err;
    if (g_wm_dpy && XDamageQueryExtension(g_wm_dpy, &base, &err)) {
        c->damage      = XDamageCreate(g_wm_dpy, plugin_win, XDamageReportBoundingBox);
        c->damage_base = base;
    }

    XFlush(dpy);

    register_capture(c);
    DEBUG_PRINT("[XW] setup parent=0x%lx plugin=0x%lx (xshm)\n",
                parent_win, plugin_win);
    return c;
}

static void cleanup_capture(Capture *c)
{
    if (!c) return;
    unregister_capture(c);
    destroy_shm(c);
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

    double clip_x1, clip_y1, clip_x2, clip_y2;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);

    for (const auto &p : c->popups) {
        if (!p.visible || p.pixmap == None) continue;

        int draw_x = p.x - c->canvas_origin_x;
        int draw_y = p.y - c->canvas_origin_y;

        // Cairo would clip this popup out correctly regardless, but only after paying
        // for the XLib surface creation below -- skip that cost entirely for popups
        // nowhere near what actually changed.
        if (draw_x + p.w <= clip_x1 || draw_x >= clip_x2 ||
            draw_y + p.h <= clip_y1 || draw_y >= clip_y2)
            continue;

        // w/h/visual are cached on the popup (set in canvas_add_popup, refreshed on
        // ConfigureNotify) -- an XGetGeometry + XGetWindowAttributes round-trip here,
        // on every single redraw of every popup, was pure X-server latency for data
        // that was already sitting in memory and does not change between redraws.
        Visual *visual = p.visual ? p.visual : DefaultVisual(c->dpy, DefaultScreen(c->dpy));
        cairo_surface_t *surf = cairo_xlib_surface_create(c->dpy, p.pixmap, visual, p.w, p.h);
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
        XTestFakeMotionEvent(c->dpy, DefaultScreen(c->dpy), x11_x, x11_y, CurrentTime);
        XFlush(c->dpy);
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

    XTestFakeMotionEvent(c->dpy, DefaultScreen(c->dpy), x11_x, x11_y, CurrentTime);
    XFlush(c->dpy);

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

    gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(da, "draw",                 G_CALLBACK(canvas_draw_cb),        c);
    g_signal_connect(da, "button-press-event",   G_CALLBACK(canvas_button_press),   c);
    g_signal_connect(da, "button-release-event", G_CALLBACK(canvas_button_release), c);
    g_signal_connect(da, "motion-notify-event",  G_CALLBACK(canvas_motion),         c);

    c->popup_canvas = win;
    c->canvas_draw  = da;
}

// The plugin's :10 origin -> desktop translation, used both to place popups and to map
// input back the other way (screen - gtk_x).
//
// Always recomputed, never cached: the plugin's origin on :10 is its slot origin, and
// the widget's origin on the desktop moves whenever the FX window moves, is docked or
// undocked, or the PLUGIN RESIZES ITSELF -- which Virtual Mix Rack does every time it
// grows to fit another module. xw_size used to store a different quantity here (the
// widget's position inside its GTK container), which happened to agree back when every
// plugin sat at :10 origin (0,0); with slots it is off by the slot origin, and a
// resize mid-drag left the drag preview badly offset.
static void refresh_gtk_offset(Capture *c, int *out_px = nullptr, int *out_py = nullptr)
{
    if (!c || !c->dpy || !c->widget) return;

    // Anchor to the CONTAINER, not to gui_win/plugin_win. The container is ours, sits
    // at the plugin's slot origin, and lives for the whole session. The plugin's own
    // windows do not: Virtual Mix Rack destroys and recreates its GUI windows when the
    // rack grows to fit another module (visible as a burst of RegisterTouchWindow in
    // the Wine log), after which gui_win/plugin_win are stale or moved and this
    // translation returns a bogus origin -- which is what threw popups off by a whole
    // window after an expansion. The container's top-left is also exactly what the
    // widget displays, so it is the correct reference regardless.
    Window child; int px = 0, py = 0;
    XTranslateCoordinates(c->dpy, c->parent_win,
                          DefaultRootWindow(c->dpy), 0, 0, &px, &py, &child);
    if (out_px) *out_px = px;
    if (out_py) *out_py = py;

    GdkWindow *ww = gtk_widget_get_window(c->widget);
    if (!ww) return;

    int wox = 0, woy = 0;
    gdk_window_get_origin(ww, &wox, &woy);
    c->gtk_x = wox - px;
    c->gtk_y = woy - py;
}

static void canvas_add_popup(Capture *c, Window x11_win, XWindowAttributes *attr)
{
    if (!c) return;

    // Update-or-insert: if this window already has an entry, reuse it and free the
    // stale pixmap (Wine reuses window IDs; a duplicate would leave a freed pixmap
    // being composited). One entry per window.
    Capture::PopupWin *pp = nullptr;
    for (auto &e : c->popups) if (e.x11_win == x11_win) { pp = &e; break; }

    // A repeated MapNotify for a window we already track, at identical geometry, is a
    // no-op re-notification (Wine re-maps, or a plugin re-confirming an in-progress
    // drag every frame while it's held over other reflowing content -- Virtual Mix
    // Rack does this while a new module is being dragged in). Treat it as one: doing
    // the full redirect/pixmap-rename/visual-fetch/offset-refresh dance on every one
    // of those, potentially many per second, was the difference between inserting a
    // module while others reflow (a burst of these) stalling and a plain move (which
    // only ever hits the already-cheap on_popup_configured) not stalling.
    if (pp && pp->pixmap != None &&
        pp->x == attr->x + c->gtk_x && pp->y == attr->y + c->gtk_y &&
        pp->w == attr->width && pp->h == attr->height)
    {
        pp->visible = true;
        if (c->popup_canvas && !gtk_widget_get_visible(c->popup_canvas))
            gtk_widget_show_all(c->popup_canvas);
        return;
    }

    DEBUG_PRINT("[DNDCOORD] canvas_add_popup win=0x%lx attr=(%d,%d,%d,%d) canvas_was_visible=%d\n",
                (unsigned long)x11_win, attr->x, attr->y, attr->width, attr->height,
                c->popup_canvas ? gtk_widget_get_visible(c->popup_canvas) : -1);

    int slot_px = 0, slot_py = 0;
    refresh_gtk_offset(c, &slot_px, &slot_py);

    if (!c->popup_canvas) create_popup_canvas(c);

    // Manual, not Automatic: Automatic also keeps the server painting the popup into
    // its parent, which made menus open/close erratically on mouse movement.
    XCompositeRedirectWindow(c->dpy, x11_win, CompositeRedirectManual);
    XFlush(c->dpy);   // non-blocking; don't stall the main loop on every popup
    Pixmap pixmap = XCompositeNameWindowPixmap(c->dpy, x11_win);

    if (pp) {
        if (pp->pixmap != None && pp->pixmap != pixmap) XFreePixmap(c->dpy, pp->pixmap);
    } else {
        c->popups.emplace_back();
        pp = &c->popups.back();
        // Report repaints of this popup so the canvas can redraw it (menu highlight
        // following the cursor, for example) without re-naming its pixmap per motion.
        if (g_wm_dpy && g_damage_event_base >= 0)
            pp->damage = XDamageCreate(g_wm_dpy, x11_win, XDamageReportBoundingBox);
    }
    pp->x11_win = x11_win;
    pp->pixmap  = pixmap;
    // attr (and attr->visual) was fetched via g_wm_dpy in on_popup_mapped -- a
    // different Display connection than c->dpy, which is what canvas_draw_cb's
    // cairo_xlib_surface_create() actually uses. A Visual* from one Xlib connection
    // is not valid to hand to a Cairo call bound to a different one, even though both
    // connect to the same X server (Xlib keeps independent client-side copies per
    // connection) -- that mismatch is what "invalid value for an input Visual*" was.
    // Fetch it fresh via c->dpy specifically, once, here at creation time: still just
    // one round-trip for this popup's entire lifetime, not one per frame.
    {
        XWindowAttributes wa_local;
        pp->visual = XGetWindowAttributes(c->dpy, x11_win, &wa_local)
                   ? wa_local.visual : DefaultVisual(c->dpy, DefaultScreen(c->dpy));
    }
    // attr->x/y are already root coordinates on :10, and gtk_x/gtk_y (= widget origin
    // on the real desktop minus the plugin's :10 origin) carry the translation to the
    // desktop. Adding px/py as well counted the plugin's :10 origin twice. That was
    // harmless while every container sat at (0,0), but each plugin now lives in its
    // own slot, so the double-count threw popups a whole slot origin away -- onto
    // another monitor. The inverse mapping used for input (screen - gtk_x) already
    // assumes this form.
    pp->x = attr->x + c->gtk_x;
    pp->y = attr->y + c->gtk_y;
    DEBUG_PRINT("[POPOFF] win=0x%lx attr=(%d,%d) slot_origin=(%d,%d) gtk=(%d,%d) -> pp=(%d,%d)\n",
                (unsigned long)x11_win, attr->x, attr->y, slot_px, slot_py,
                c->gtk_x, c->gtk_y, pp->x, pp->y);
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
    DEBUG_PRINT("[DNDCOORD] canvas_remove_popup win=0x%lx remaining_before=%zu\n",
                (unsigned long)x11_win, c->popups.size());
    for (auto it = c->popups.begin(); it != c->popups.end(); ) {
        if (it->x11_win == x11_win) {
            if (it->damage && g_wm_dpy) { XDamageDestroy(g_wm_dpy, it->damage); it->damage = 0; }
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
    Visual *visual = nullptr;
    int gw = 0, gh = 0;
    for (auto &md : m->cap->modals)
        if (md.x11_win == m->x11_win) { pm = md.pixmap; visual = md.visual; gw = md.w; gh = md.h; break; }
    if (!pm) return false;

    // w/h/visual are cached on the ModalWin (set once in create_modal) -- this used to
    // redo an XGetGeometry + XGetWindowAttributes round-trip on every single redraw,
    // the same bug already found and fixed for popups (see canvas_draw_cb).
    if (!visual) visual = DefaultVisual(dpy, DefaultScreen(dpy));

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
    XCompositeRedirectWindow(dpy, win, CompositeRedirectManual);
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
    md.w = attr->width; md.h = attr->height;
    // attr (and attr->visual) was fetched via g_wm_dpy in on_popup_mapped -- a
    // different Xlib connection than dpy, which is what modal_draw_cb's
    // cairo_xlib_surface_create() actually uses. A Visual* from one connection isn't
    // valid on another (same mismatch found and fixed for popups earlier -- see
    // canvas_add_popup). Fetch it fresh via dpy specifically, once, here.
    {
        XWindowAttributes wa_local;
        md.visual = XGetWindowAttributes(dpy, win, &wa_local)
                  ? wa_local.visual : DefaultVisual(dpy, DefaultScreen(dpy));
    }
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

static bool is_xdnd_icon_window(Display *dpy, Window win)
{
    Atom atom_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom atom_type_dnd    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DND", False);
    Atom actual_type; int actual_format; unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = nullptr;
    bool is_dnd = false;
    if (XGetWindowProperty(dpy, win, atom_window_type, 0, 10, False, XA_ATOM,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success && prop)
    {
        if (actual_format == 32 && nitems > 0) {
            Atom *atoms = (Atom*)prop;
            for (unsigned long i = 0; i < nitems; i++)
                if (atoms[i] == atom_type_dnd) { is_dnd = true; break; }
        }
        XFree(prop);
    }
    return is_dnd;
}

static void handle_new_window(Window win, Capture *state, XWindowAttributes *attr) {
    if (!state->dpy || !win || !state) return;

    // Already tracked? A repeated MapNotify (Wine re-maps) must not spawn a second
    // popup/modal. Refresh an existing popup; leave an existing modal alone.
    for (auto &p : state->popups)
        if (p.x11_win == win) { canvas_add_popup(state, win, attr); return; }
    for (auto &m : state->modals)
        if (m.x11_win == win) return;

    // A _NET_WM_WINDOW_TYPE_DND window is an XDND drag-icon window -- yabridge keeps
    // one mapped for the whole plugin-drag-out XDND session (see the catcher in
    // xwayland-bridge-wm.cpp). classify_popup() correctly classifies it as a popup
    // per EWMH, but routing it into canvas_add_popup mapped the full-screen,
    // always-on-top overlay meant for the plugin's own dropdown/tooltip popups for
    // the entire drag, which sat directly on top of REAPER's native drag-preview
    // tooltip everywhere on screen. This window is never meant to be user-visible
    // (:10 is a headless Xvfb server, and the drag feedback the user actually sees is
    // REAPER's own native drag), so ignore it rather than composite it.
    if (is_xdnd_icon_window(state->dpy, win)) {
        DEBUG_PRINT("[DNDCOORD] ignoring XDND icon window 0x%lx (not compositing as popup)\n", (unsigned long)win);
        return;
    }

    bool is_popup = classify_popup(state->dpy, win, attr);
    if (is_popup) {
        if (attr->width <= 1 || attr->height <= 1) {
            DEBUG_PRINT("skipping 1x1 window: 0x%lx (%dx%d)\n", (unsigned long)win, attr->width, attr->height);
            return;
        }
        if (attr->width == 12 || attr->height == 12) {
            DEBUG_PRINT("skipping unnamed shadow juce window: 0x%lx (%dx%d)\n", (unsigned long)win, attr->width, attr->height);
            return;
        }
        canvas_add_popup(state, win, attr);
    } else {
        create_modal(state, win, attr);
    }
}

static void on_popup_mapped(Window w)
{
    XWindowAttributes attr;
    if (!XGetWindowAttributes(g_wm_dpy, w, &attr) || attr.map_state != IsViewable)
        return;

    DEBUG_PRINT("[SLOTATTR] win=0x%lx attr=(%d,%d)\n", (unsigned long)w, attr.x, attr.y);
    for (auto &kv : g_captures) {
        Capture *cand = kv.second;
        if (!cand) continue;
        int sx, sy, sw, sh;
        slot_rect(cand->slot, &sx, &sy, &sw, &sh);
        DEBUG_PRINT("[SLOTATTR]   candidate slot=%d rect=(%d,%d,%d,%d) contains=%d\n",
                    cand->slot, sx, sy, sw, sh, point_in_slot(cand->slot, attr.x, attr.y));
    }

    // Prefer geometric attribution: which instance's slot does this popup's position
    // actually fall inside? PID matching alone can't tell apart multiple instances of
    // the same plugin sharing one Wine host process (a real yabridge/Wine behaviour,
    // used to save memory/startup time) -- every popup from every such instance
    // shares the same PID, so PID-only attribution always picked whichever instance
    // happened to be first in g_captures, regardless of which one the popup actually
    // came from. Override-redirect popups (what these are, by ICCCM convention) are
    // reparented directly under root rather than under the plugin's own container,
    // so there's no X11 hierarchy to walk back through either -- but the slot system
    // already guarantees each instance a unique, non-overlapping rectangle on :10.
    for (auto &kv : g_captures) {
        Capture *cand = kv.second;
        if (cand && point_in_slot(cand->slot, attr.x, attr.y) &&
            is_window_from_owned_plugin(cand->dpy, w, cand->gui_win)) {
            DEBUG_PRINT("[SLOTATTR]   -> matched by slot, cand->slot=%d\n", cand->slot);
            handle_new_window(w, cand, &attr);
            return;
        }
    }
    // Fallback: no candidate's slot contains this position -- e.g. slot allocation
    // exhausted (more than 32 instances open) and several instances share the
    // (XW_SLOT_MARGIN, XW_SLOT_MARGIN) fallback position from xw_alloc_slot. Fall
    // back to PID-only matching rather than dropping the popup entirely.
    DEBUG_PRINT("[SLOTATTR]   -> no slot matched, falling back to PID-only\n");
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
// configure coords and re-capture its pixmap.
static bool on_popup_configured(Window w, int cx, int cy, int cw, int ch)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        for (auto &p : c->popups) {
            if (p.x11_win != w) continue;
            // cx/cy are root coordinates on :10, so they need the same translation
            // canvas_add_popup applies -- p.x/p.y are desktop coordinates for the
            // canvas. Storing them raw put the popup a whole slot origin off (617 ->
            // 2665), far outside the canvas, so a moving popup vanished on its first
            // ConfigureNotify. Harmless before slots, when containers sat at (0,0)
            // and the two coordinate spaces happened to coincide. Menus never hit
            // this because they do not move; the drag preview moves constantly.
            // A moving popup can outlive a plugin resize (Virtual Mix Rack expands as
            // modules are added), so refresh before translating rather than trusting
            // whatever the offset was when the popup first appeared.
            int ox = 0, oy = 0;
            refresh_gtk_offset(c, &ox, &oy);
            // Some plugins mix coordinate frames. Virtual Mix Rack sends the drag
            // preview's x in ROOT space (it follows the cursor, which we warp into the
            // plugin's slot) but its y in its own CLIENT space, taken from the rack
            // layout: cfg=2207,75 against a container at 2048,2048. On a normal host
            // the window sits near screen origin so the two frames coincide and nobody
            // notices; in a slot at 2048,2048 the y lands a whole slot origin above the
            // widget and the preview vanishes off-screen.
            //
            // A window living in this slot cannot legitimately be positioned above or
            // left of the container, so treat such a coordinate as client-relative and
            // rebase it. Root coordinates are left untouched.
            //
            // ox,oy here used to be a second, separate XTranslateCoordinates call
            // duplicating exactly what refresh_gtk_offset() just computed above. A
            // moving popup (the drag preview) fires this on every ConfigureNotify --
            // one per mouse-move tick while dragging -- so that was two X round-trips
            // doing the same translation, every single tick. refresh_gtk_offset() now
            // hands its result back directly instead.
            int old_x = p.x, old_y = p.y, old_w = p.w, old_h = p.h;
            {
                int fx = cx, fy = cy;
                if (fx < ox) fx += ox;
                if (fy < oy) fy += oy;
                p.x = fx + c->gtk_x;
                p.y = fy + c->gtk_y;
            }
            // A pure move (size unchanged) leaves the existing composite pixmap's
            // content perfectly valid -- only re-name it when the size actually
            // changes. Freeing and re-creating it unconditionally on every
            // ConfigureNotify meant a moving drag-preview popup paid for an
            // XFreePixmap + XCompositeNameWindowPixmap round-trip on every tick, for
            // no visual benefit (the window's pixels didn't change, just its position).
            bool size_changed = (p.w != cw || p.h != ch);
            p.w = cw; p.h = ch;
            if (size_changed) {
                if (p.pixmap != None) XFreePixmap(c->dpy, p.pixmap);
                p.pixmap = XCompositeNameWindowPixmap(c->dpy, w);
            }
            if (c->canvas_draw) {
                GdkWindow *cwin = gtk_widget_get_window(c->canvas_draw);
                // This used to invalidate the whole canvas (NULL rect) -- a full
                // monitor-sized clear-and-recomposite on every single position tick of
                // a moving popup. The damage (content-change) handler got scoped to
                // its actual rect last round; this position-change path, which is what
                // actually fires continuously while dragging or during Virtual Mix
                // Rack's reflow animation, was still invalidating everything. Scope it
                // to the old rect (erase the stale paint at the previous position) and
                // the new rect (paint the fresh content at the new one).
                if (cwin) {
                    GdkRectangle old_r = { old_x - c->canvas_origin_x, old_y - c->canvas_origin_y, old_w, old_h };
                    GdkRectangle new_r = { p.x  - c->canvas_origin_x, p.y  - c->canvas_origin_y, p.w,   p.h   };
                    gdk_window_invalidate_rect(cwin, &old_r, FALSE);
                    gdk_window_invalidate_rect(cwin, &new_r, FALSE);
                } else {
                    gtk_widget_queue_draw(c->canvas_draw);
                }
            }
            return true;
        }
    }
    return false;
}

// Bridge per-event handling, registered as g_wm->on_unhandled_event. The WM has
// already processed the event (MapRequest/ConfigureRequest/etc, plus the XDND
// catcher protocol -- see xwayland-bridge-wm.cpp); here we deal only with our
// own concerns: popup/modal lifecycle, damage-driven capture, and starting the
// native SWELL drag once the WM has a drag-out payload (see on_motion above).
static void bridge_handle_event(XEvent *ev)
{
    // The WM's handle_event() already consumed the XDND catcher's own protocol
    // traffic (XFixes/ClientMessage/SelectionNotify) for anything addressed to the
    // catcher window. That consumption doesn't stop on_unhandled_event from firing
    // regardless -- it fires for every event -- so we still have to actively skip
    // the catcher's own events here ourselves, or two things end up answering the
    // same XDND message: the WM's catcher (accept / finished-success) and the
    // legacy refuse-everything ClientMessage handler further down in this function
    // (refuse / finished-not-accepted). yabridge got both replies to the same
    // XdndPosition/XdndDrop, which is what left it unable to start a second drag.
    Window dnd_catcher = g_wm ? g_wm->dnd_catcher_window() : 0;
    if (dnd_catcher &&
        ((ev->type == MapNotify   && ev->xmap.window   == dnd_catcher) ||
         (ev->type == UnmapNotify && ev->xunmap.window == dnd_catcher) ||
         (ev->type == ClientMessage && ev->xclient.window == dnd_catcher)))
        return;   // our own proxy window: the WM layer already answered this

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

        // Helper lambda to safely update the shared memory image for a given capture context
        auto update_capture_buffer = [](Capture *c) -> bool {
            if (!c || !c->dpy || !c->plugin_win) return false;

            XWindowAttributes wa;
            if (!XGetWindowAttributes(c->dpy, c->plugin_win, &wa)) return false;
            if (wa.width <= 0 || wa.height <= 0) return false;
            if (!ensure_shm(c, wa.width, wa.height)) return false;

            return XShmGetImage(c->dpy, c->plugin_win, c->shm_img, 0, 0, AllPlanes);
        };

        // Plugin GUI: de->area is in plugin_win coords, which map 1:1 to the
        // widget (on_draw blits the pixmap at 0,0). cairo clips the paint to this
        // rect, so only this region blits. Modals work the same way against md.draw.
        Capture *c = find_capture(de->drawable);
        if (c && c->widget && GTK_IS_WIDGET(c->widget)) {
            // Update the buffer from the X Server immediately before scheduling the GTK draw
            if (update_capture_buffer(c)) {
                c->has_painted = true;
                gtk_widget_queue_draw_area(c->widget, de->area.x, de->area.y,
                                           de->area.width, de->area.height);
            }
            return;
        }

        for (auto &kv : g_captures) {
            Capture *cc = kv.second;
            if (!cc) continue;

            // Popups: damage on a popup (menu highlight following the cursor, for
            // instance) has to repaint the canvas it is composited into. Without
            // this the canvas only updated as a side effect of pixmap churn in
            // canvas_motion, which flickered.
            //
            // update_capture_buffer() was being called here too, but it operates on
            // c->plugin_win -- the MAIN plugin GUI's own SHM buffer, which has nothing
            // to do with a popup. Every popup damage event (e.g. a menu highlight
            // following the cursor, which can fire continuously while hovering) was
            // paying for a full refresh of the main plugin canvas as an unrelated side
            // effect. The popup's own content comes straight from its composited
            // pixmap via Cairo/XLib in canvas_draw_cb -- no separate buffer to update.
            //
            // Likewise this used to invalidate the whole canvas widget, which is sized
            // to the full screen (create_popup_canvas). A tiny highlight change inside
            // a small dropdown was forcing GTK to clear-and-recomposite a full
            // monitor-sized transparent surface every time. Map the damaged sub-rect
            // into canvas-local coordinates instead, matching the precision already
            // used for the plugin GUI and modal paths just above.
            for (auto &pp : cc->popups) {
                if (pp.x11_win == de->drawable) {
                    if (cc->canvas_draw && GTK_IS_WIDGET(cc->canvas_draw)) {
                        int inv_x = pp.x - cc->canvas_origin_x + de->area.x;
                        int inv_y = pp.y - cc->canvas_origin_y + de->area.y;
                        gtk_widget_queue_draw_area(cc->canvas_draw, inv_x, inv_y,
                                                   de->area.width, de->area.height);
                    }
                    return;
                }
            }

            for (auto &md : cc->modals) {
                if (md.x11_win == de->drawable) {
                    if (md.draw && GTK_IS_WIDGET(md.draw)) {
                        if (update_capture_buffer(cc)) {
                            gtk_widget_queue_draw_area(md.draw, de->area.x, de->area.y,
                                                       de->area.width, de->area.height);
                        }
                    }
                    return;
                }
            }
        }
        return;
    }


    // ---- XDND coming FROM the plugin (dragging something out of it) ----
    // Wine runs a nested drag loop while dragging. Inside it, it sends XdndEnter /
    // XdndPosition to the window under the pointer -- our container -- and then WAITS
    // for an XdndStatus reply. Nothing here answered: this switch only handled
    // map/unmap/configure, and the find_capture() check below returns early for the
    // container anyway. So the loop blocked forever, the plugin could not service
    // yabridge, and REAPER's next host->plugin call parked its UI thread in recv() --
    // the same deadlock shape as the modal-dialog hang.
    //
    // Answering is what matters, not accepting. We currently REFUSE (accept bit 0):
    // the drag ends cleanly with a no-drop cursor instead of hanging. Accepting would
    // additionally require handling XdndDrop plus the XdndSelection transfer, and
    // leaving that half-done would hang again waiting for XdndFinished.
    if (ev->type == ClientMessage)
    {
        Display *dpy   = ev->xclient.display;
        const Atom mt  = ev->xclient.message_type;
#ifdef _DEBUG
        {   // DIAG: does the plugin's drag handshake reach us at all?
            char *n = XGetAtomName(dpy, mt);
            DEBUG_PRINT("[DNDX] ClientMessage %s win=0x%lx src=0x%lx\n",
                        n ? n : "?", ev->xclient.window,
                        (unsigned long)ev->xclient.data.l[0]);
            if (n) XFree(n);
        }
#endif
        const Window self = ev->xclient.window;
        const Window src  = (Window)ev->xclient.data.l[0];

        if (src && mt == XInternAtom(dpy, "XdndPosition", False))
        {
            XClientMessageEvent st; memset(&st, 0, sizeof(st));
            st.type         = ClientMessage;
            st.display      = dpy;
            st.window       = src;
            st.message_type = XInternAtom(dpy, "XdndStatus", False);
            st.format       = 32;
            st.data.l[0]    = (long)self;
            st.data.l[1]    = 0;      // bit 0 clear = will not accept a drop
            st.data.l[2]    = 0;      // no "silent" rectangle: keep sending positions
            st.data.l[3]    = 0;
            st.data.l[4]    = None;   // no action
            XSendEvent(dpy, src, False, NoEventMask, (XEvent*)&st);
            XFlush(dpy);
            return;
        }
        if (src && mt == XInternAtom(dpy, "XdndDrop", False))
        {
            // Should not arrive while we refuse, but answer anyway -- an unanswered
            // XdndDrop leaves the source waiting on XdndFinished, which is a hang.
            XClientMessageEvent fin; memset(&fin, 0, sizeof(fin));
            fin.type         = ClientMessage;
            fin.display      = dpy;
            fin.window       = src;
            fin.message_type = XInternAtom(dpy, "XdndFinished", False);
            fin.format       = 32;
            fin.data.l[0]    = (long)self;
            fin.data.l[1]    = 0;     // not accepted
            fin.data.l[2]    = None;
            XSendEvent(dpy, src, False, NoEventMask, (XEvent*)&fin);
            XFlush(dpy);
            return;
        }
        // XdndEnter and XdndLeave require no reply.
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
// Dismiss any open plugin popup for this capture by clicking outside it on :10.
//
// This replaces sending Escape. A Wine menu holds a POINTER grab, and keyboard focus
// on :10 is not on the menu, so the key event never reached it -- Escape silently did
// nothing and the grab stayed, which is what locks REAPER's input up entirely. A click
// outside the menu is what actually dismisses it; the grab owner consumes that click
// to close itself, so no control underneath gets activated.
static void dismiss_popups_by_click(Capture *c)
{
    if (!c || !c->dpy || c->popups.empty()) return;

    Window rootw = DefaultRootWindow(c->dpy);
    Window childw; int px = 0, py = 0;
    XTranslateCoordinates(c->dpy, c->gui_win ? c->gui_win : c->plugin_win,
                          rootw, 0, 0, &px, &py, &childw);

    // Top-left corner of the plugin: a menu opened from a control is essentially
    // never covering it. If one is, aim just below that popup instead.
    int ax = px + 1, ay = py + 1;
    for (const auto &p : c->popups) {
        if (!p.visible) continue;
        const int p10x = p.x - c->gtk_x, p10y = p.y - c->gtk_y;
        if (ax >= p10x && ax < p10x + p.w && ay >= p10y && ay < p10y + p.h)
            ay = p10y + p.h + 1;
    }

    XTestFakeMotionEvent(c->dpy, DefaultScreen(c->dpy), ax, ay, CurrentTime);
    XFlush(c->dpy);
    XTestFakeButtonEvent(c->dpy, Button1, True,  CurrentTime);
    XTestFakeButtonEvent(c->dpy, Button1, False, CurrentTime);
    XFlush(c->dpy);
}

bool xw_bridge_swell_on_button_event_escape()
{
    bool any = false;
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c) continue;
        // Raise the modal so an outside click brings it back to the front rather than
        // leaving it stranded behind the plugin.
        if (!c->modals.empty()){
            xw_raise_modals();
            any = true;
        }
        if (!c->popups.empty()){
            // Runs when the user clicks anywhere in REAPER, including left of or
            // above the plugin where the overlay canvas does not reach (it is an
            // xdg_popup anchored to the FX window, so it only extends right and down
            // and those clicks never hit canvas_button_press).
            dismiss_popups_by_click(c);
            any = true;
        }
    }
    return any;
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
            // Tell the plugin to close its menu FIRST, while we still have the popup
            // list -- dismiss_popups_by_click() returns immediately if popups is empty,
            // so doing this after the clear below (as the `if (any)` block used to) was
            // a no-op. Wine was left holding the menu's pointer grab as its window was
            // destroyed, which wedges the plugin and hangs REAPER. Modals are fine
            // because they take no pointer grab.
            dismiss_popups_by_click(c);
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
        if (!c->modals.empty() && c->dpy) {
            // Modals must be dismissed too. Previously this branch only *checked* for
            // an active modal, so Super+Q left the dialog open and, because the check
            // also gated `any`, the close did nothing at all -- leaving a plugin with
            // an open dialog that hangs the moment REAPER regains focus.
            //
            // A dialog cannot be dismissed the way a popup is: a synthetic click would
            // press whatever button has focus, and Escape needs :10 keyboard focus we
            // cannot guarantee. WM_DELETE_WINDOW is the protocol-correct "please
            // close" and Wine dialogs honour it regardless of focus.
            const Atom a_prot = XInternAtom(c->dpy, "WM_PROTOCOLS", False);
            const Atom a_del  = XInternAtom(c->dpy, "WM_DELETE_WINDOW", False);
            for (auto &md : c->modals) {
                if (!md.x11_win) continue;
                any = true;
                XClientMessageEvent m; memset(&m, 0, sizeof(m));
                m.type         = ClientMessage;
                m.display      = c->dpy;
                m.window       = md.x11_win;
                m.message_type = a_prot;
                m.format       = 32;
                m.data.l[0]    = (long)a_del;
                m.data.l[1]    = CurrentTime;
                XSendEvent(c->dpy, md.x11_win, False, NoEventMask, (XEvent*)&m);
            }
            XFlush(c->dpy);
        }
    }
    // Only touch :10 / send Escape when there was actually a popup or modal to
    // dismiss. Firing Escape on EVERY window close — including a plain plugin close
    // with nothing open — is what corrupts the teardown and crashes Wine (the
    // plugin's GUI window gets destroyed from the outside -> BadWindow).
    if (any) {
        // gdk_display_flush(gdk_display_get_default());
        // Click outside the popup rather than sending Escape: the menu holds a pointer
        // grab and never receives the key, so Escape left the grab in place and locked
        // input up. Modals are separate GTK windows with their own key handling and
        // are unaffected by this.
        // (popup dismissal now happens above, before the list is cleared)
    }
    return any;
}

// Ground-truth dump of every capture's overlay/modal state -- what actually exists
// right now, regardless of which code path we think created it. Callable from
// anywhere via xwayland-bridge.h.
void xw_bridge_debug_dump_overlays()
{
    DEBUG_PRINT("[DNDCOORD] --- overlay dump: %zu captures ---\n", g_captures.size());
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c) continue;
        bool canvas_vis = c->popup_canvas && GTK_IS_WIDGET(c->popup_canvas) && gtk_widget_get_visible(c->popup_canvas);
        int cx = -1, cy = -1;
        if (canvas_vis)
        {
            GdkWindow *gw = gtk_widget_get_window(c->popup_canvas);
            if (gw) gdk_window_get_origin(gw, &cx, &cy);
        }
        DEBUG_PRINT("[DNDCOORD]   capture hwnd=%p popup_canvas=%p visible=%d origin=(%d,%d) size=(%d,%d) popups=%zu modals=%zu\n",
                    (void*)c->hwnd, (void*)c->popup_canvas, canvas_vis, cx, cy,
                    c->canvas_w, c->canvas_h, c->popups.size(), c->modals.size());
    }
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
    g_wm->dnd_init();
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
             c->slot = bs->slot;

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

             // See ensure_shm/on_draw/update_capture_buffer above: our very first
             // paint just blits whatever is already in the SHM buffer, which only
             // gets populated reactively from a real DamageNotify. Xvfb is headless,
             // so the automatic "you're now visible, please paint yourself" Expose a
             // real display generates may never reach Wine, leaving some widgets
             // never actually painted (hence never damaged, hence never captured)
             // until an incidental resize or hover nudges that specific area into
             // repainting itself. Force it explicitly instead of relying on that.
             XClearArea(bs->disp, plugin_win, 0, 0, 0, 0, True);
             if (c->gui_win != plugin_win)
                 XClearArea(bs->disp, c->gui_win, 0, 0, 0, 0, True);
             XFlush(bs->disp);
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
    xw_free_slot(bs->slot);
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

    // Refresh the popup offset so popups position correctly in both floating and
    // FX-list embedded modes. Must use the same computation as everywhere else --
    // assigning pos_x/pos_y here (the widget's position inside its container) stored a
    // different quantity and offset every popup after a resize.
    // if (bs->cap) refresh_gtk_offset(bs->cap);

    // Re-capture the pixmap at the new size — xw_size runs exactly when the window
    // is being resized, so the backing pixmap must be refreshed here. Guard on
    // bs->cap: WM_SIZE/SetWindowPos can fire before the plugin is captured (e.g.
    // during an FX-list swap), and the container resize also needs a live capture.
    if (bs->cap) {
        XResizeWindow(bs->cap->dpy, bs->cap->parent_win, w, h);
        XFlush(bs->cap->dpy);
    }

    if (bs->cap) refresh_gtk_offset(bs->cap);
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

// Every plugin container used to be created at (0,0), so on :10 they all stacked on
// top of each other. Input is delivered with XTest, which goes to whatever window is
// under the pointer -- so with the containers overlapping, every click landed on the
// topmost one and only the last-opened plugin responded. That is the input "dead
// zone". On a real display there is nowhere else to put them; on Xvfb's large virtual
// framebuffer each plugin simply gets its own slot, and then the pointer genuinely is
// over the intended window. Nothing on this display is ever visible, so the layout
// only has to avoid overlap -- which also matters because overlapping windows lose
// the occluded pixels when captured.
#define XW_SLOT_W      2048
#define XW_SLOT_H      1536
#define XW_SLOT_COLS   3
// No slot touches the screen edges. A knob near a plugin's top-left needs the pointer
// to keep travelling up/left while dragging, and at origin (0,0) it just clamps at the
// edge and stops producing motion, so the knob freezes. The margin gives every plugin
// room on all sides -- free on a framebuffer this size.
#define XW_SLOT_MARGIN 2048
static unsigned int g_slot_used;   // bitmask of occupied slots

static int xw_alloc_slot(int *sx, int *sy)
{
    for (int i = 0; i < 32; i++)
    {
        if (g_slot_used & (1u << i)) continue;
        g_slot_used |= (1u << i);
        *sx = XW_SLOT_MARGIN + (i % XW_SLOT_COLS) * XW_SLOT_W;
        *sy = XW_SLOT_MARGIN + (i / XW_SLOT_COLS) * XW_SLOT_H;
        return i;
    }
    *sx = *sy = XW_SLOT_MARGIN;   // out of slots: at least stay off the edges
    return -1;
}

static void xw_free_slot(int slot)
{
    if (slot >= 0 && slot < 32) g_slot_used &= ~(1u << slot);
}

// Does a raw :10 position fall inside the rectangle a given slot owns? Used to tell
// which instance a newly-mapped popup actually belongs to, since PID alone can't
// distinguish multiple instances of the same plugin sharing one Wine host process
// (see is_window_from_owned_plugin's caller in on_popup_mapped) and override-redirect
// popups aren't reparented under the plugin's own container, so there's no X11
// hierarchy to walk back either. The slot system already guarantees every instance a
// unique, non-overlapping rectangle, so this is a reliable discriminator using
// information we already have.
static void slot_rect(int slot, int *sx, int *sy, int *sw, int *sh)
{
    *sx = (slot >= 0) ? (XW_SLOT_MARGIN + (slot % XW_SLOT_COLS) * XW_SLOT_W) : -1;
    *sy = (slot >= 0) ? (XW_SLOT_MARGIN + (slot / XW_SLOT_COLS) * XW_SLOT_H) : -1;
    *sw = XW_SLOT_W;
    *sh = XW_SLOT_H;
}

static bool point_in_slot(int slot, int x, int y)
{
    if (slot < 0) return false;
    int sx, sy, sw, sh;
    slot_rect(slot, &sx, &sy, &sw, &sh);
    return x >= sx && x < sx + sw && y >= sy && y < sy + sh;
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

    int slot_x = 0, slot_y = 0;
    const int slot = xw_alloc_slot(&slot_x, &slot_y);

    Window container = XCreateSimpleWindow(disp, root, slot_x, slot_y, w, h, 0,
                                           BlackPixel(disp, screen),
                                           WhitePixel(disp, screen));
    XMapWindow(disp, container);
    XFlush(disp);

    GtkWidget *draw_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area, w, h);

    hwnd = new HWND__(viewpar, 0, r, NULL, true, xw_bridgeProc);
    hwnd->m_classname = bridge_class_name;
    hwnd->m_oswidget  = draw_area;

    g_object_add_weak_pointer(G_OBJECT(draw_area), (gpointer*)&hwnd->m_oswidget);

    bridgeState *bs = new bridgeState();
    bs->disp   = disp;
    bs->parent = container;
    bs->slot   = slot;
    hwnd->m_private_data = (INT_PTR)bs;

    *wref = (void*)container;
    SetTimer(hwnd, 1, 100, NULL);
    return hwnd;
}

// Called from SWELL OnKeyEvent
// See the header. Consumes keys while any plugin modal dialog is open.
//
// Backtrace of the hang this prevents:
//   OnKeyEvent -> SWELLAppMain(WM_KEYDOWN) -> REAPER -> libyabridge-vst2 -> recv()
// REAPER dispatches the key into the plugin, yabridge waits synchronously for the
// Wine-side host to answer, and that host is inside its modal dialog's nested message
// loop and never will. The UI thread blocks in recv() and REAPER has to be killed.
// Focus, grabs and Escape were all irrelevant -- the key simply must not reach the
// plugin while a modal is up. Sending it to the dialog instead also lets Escape close
// the dialog, which releases the nested loop.
bool xw_bridge_forward_key_to_modal(int keycode, int state, bool is_press)
{
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c || !c->dpy || c->modals.empty()) continue;

        Capture::ModalWin &md = c->modals.back();
        Window target = md.x11_win;
        if (!target) continue;
        xw_raise_modals();
        return true;
    }
    return false;
}

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
