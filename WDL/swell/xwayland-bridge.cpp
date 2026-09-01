#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif
#include "xwayland-bridge.h"
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <cairo/cairo-xlib.h>
void xw_size(HWND hwnd);

XWaylandWM     *g_wm           = nullptr;
Display *g_wm_dpy       = nullptr;


struct Capture {
    Display   *dpy         = nullptr;
    Window     parent_win  = 0;
    Window     plugin_win  = 0;
    Window     gui_win     = 0;
    Pixmap     pixmap      = 0;      // XComposite-named window pixmap; refreshed on resize
    int        pixmap_w    = 0;
    int        pixmap_h    = 0;
    Visual    *visual      = nullptr; // plugin_win's visual, cached for cairo_xlib_surface_create
    int        depth       = 0;
    GtkWidget *widget      = nullptr;
    GtkWidget *active_toplevel = nullptr; // toplevel we hooked notify::is-active on; disconnected in cleanup
    HWND       hwnd        = nullptr;
    //
    Damage     damage      = 0;
    int        damage_base = 0;

    GtkWidget *popup_canvas    = nullptr;
    GtkWidget *canvas_draw     = nullptr;
    int        canvas_origin_x = 0;
    int        canvas_origin_y = 0;
    int        canvas_w        = 0;
    int        canvas_h        = 0;
    int        gtk_x           = 0;
    int        gtk_y           = 0;
    Window     root_popup      = 0;


    struct PopupWin { Window x11_win; Pixmap pixmap; int x,y,w,h; bool visible; Damage damage; Visual *visual; };
    std::vector<PopupWin> popups;

    // ── Modals (real dialog windows the plugin opens) ──
    struct ModalWin { Window x11_win; Pixmap pixmap; GtkWidget *gtk_win; GtkWidget *draw; Damage damage; int w; int h; Visual *visual; };
    std::vector<ModalWin> modals;
};

static std::map<Window, Capture*> g_captures;
static int g_damage_event_base = -1;
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

// The currently-active capture, i.e. the plugin the user is actually
// interacting with. Set eagerly on button-press (before we forward the click
// to X, so a synchronously-mapped popup doesn't race us) and by the
// compositor's own focus signal on our top-level (GTK's notify::is-active)
// for the general case.
Capture *g_active_capture = nullptr;


struct bridgeState {
    Display *disp   = nullptr;   // this plugin's connection to :10
    Window   parent = 0;         // container window on :10
    Capture *cap    = nullptr;
    bool     placed = false;     // has the SWELL widget been put in its container
    RECT     last_size_pos = {0,0,0,0}; // last hwnd->m_position xw_size actually acted on, see xw_size
    bool     has_last_size_pos = false;
    GtkWidget *embed_container = nullptr; // resolved via xw_ensure_embed_widget, see xw_bridge_create/xw_size
    // Host toplevel our widget is currently placed in. Mirrors bs->cur_parent
    // in SWELL's X11 bridge (swell-generic-gdk.cpp, xbridgeProc WM_SIZE):
    // REAPER destroys and recreates the FX window when a plugin resizes
    // itself, and the X11 bridge detects that by comparing this against the
    // host's current m_oswindow and reparenting into the new one. Without the
    // equivalent here, our widget is left in the discarded toplevel -- alive
    // at the GTK level, but with its GdkWindow destroyed, so it never draws.
    GtkWidget *cur_parent_toplevel = nullptr;
};

// Refresh the composite-named window pixmap after the plugin window has been
// (re)configured. Automatic redirection means the X server keeps drawing the
// window off-screen; we sample it via XCompositeNameWindowPixmap and render
// with cairo's xlib backend. The pixmap handle becomes invalid on every
// resize, so we re-name it whenever the size we saw changes.
static bool ensure_pixmap(Capture *c, int w, int h)
{
    if (c->pixmap && c->pixmap_w == w && c->pixmap_h == h) return true;

    if (!c->visual) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(c->dpy, c->plugin_win, &wa)) return false;
        c->visual = wa.visual;
        c->depth  = wa.depth;
    }

    // Order matters: flush any pending server-side resize first, then rename.
    // Otherwise the newly-named pixmap gets invalidated by the resize we just
    // missed and the next free hits BadPixmap.
    XFlush(c->dpy);

    Pixmap old = c->pixmap;
    c->pixmap  = XCompositeNameWindowPixmap(c->dpy, c->plugin_win);
    if (old) XFreePixmap(c->dpy, old);

    c->pixmap_w = w;
    c->pixmap_h = h;
    return c->pixmap != 0;
}

static void destroy_pixmap(Capture *c)
{
    if (!c) return;
    if (c->pixmap && c->dpy) { XFreePixmap(c->dpy, c->pixmap); c->pixmap = 0; }
    c->pixmap_w = c->pixmap_h = 0;
}

// Unconditional composite-pixmap rename. Called whenever we know the pixmap
// handle is about to become invalid (resize) or already has. Does not skip on
// unchanged size -- ensure_pixmap does that when size is the guarantee we
// have; refresh_pixmap is the one to use when the caller knows a resize is in
// flight.
static void refresh_pixmap(Capture *c)
{
    if (!c || !c->dpy || !c->plugin_win) return;

    // Flush any pending server-side resize before naming, otherwise the
    // freshly-named pixmap gets invalidated by the resize we just missed and
    // the next XFreePixmap hits BadPixmap.
    XFlush(c->dpy);

    Pixmap old = c->pixmap;
    c->pixmap  = XCompositeNameWindowPixmap(c->dpy, c->plugin_win);
    if (old) XFreePixmap(c->dpy, old);

    // Track size for ensure_pixmap's fast path.
    XWindowAttributes wa;
    if (XGetWindowAttributes(c->dpy, c->plugin_win, &wa)) {
        c->pixmap_w = wa.width;
        c->pixmap_h = wa.height;
        if (!c->visual) { c->visual = wa.visual; c->depth = wa.depth; }
    }

    if (c->widget && GTK_IS_WIDGET(c->widget)) gtk_widget_queue_draw(c->widget);
}

static bool on_draw(GtkWidget *, cairo_t *cr, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->pixmap || !c->visual) return FALSE;

    // Wrap the composite-named pixmap directly as a cairo xlib surface --
    // no capture/copy step. The X server keeps this pixmap in sync with the
    // off-screen plugin window because we requested Automatic redirection.
    cairo_surface_t *surf = cairo_xlib_surface_create(c->dpy, c->pixmap,
                                                      c->visual,
                                                      c->pixmap_w, c->pixmap_h);
    if (surf) {
        if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
            cairo_set_source_surface(cr, surf, 0, 0);
            cairo_paint(cr);
        }
        cairo_surface_destroy(surf);
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
    // Update active BEFORE forwarding the click. The plugin may synchronously
    // spawn a popup in response, and if we relied on notify::is-active to
    // update g_active_capture that signal wouldn't have fired yet -- the popup
    // would map first and be misattributed to the previously-active capture.
    // Button-press is the earliest reliable "user just started interacting
    // with THIS plugin" signal we have.
    g_active_capture = c;
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
    if (g_wm && g_wm->dnd_has_pending()) {
        static char path[8192];
        g_wm->dnd_take_pending_path(path, sizeof(path));
        if (path[0]) {
            DEBUG_PRINT("[DNDX] starting native drag: %s\n", path);
            Capture *cc = (Capture*)data;
            const char *lst[1] = { path };
            SWELL_InitiateDragDropOfFileList(cc ? cc->hwnd : NULL, NULL, lst, 1, NULL);
            DEBUG_PRINT("[DNDX] native drag finished\n");

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


// Compositor told GTK that our top-level's is-active state changed. Under
// normal Wayland behaviour is-active only becomes true when the user has
// actually clicked/focused the window, so this is the right signal for
// "user wants to interact with this plugin".
//
// In floating mode there's one capture per top-level, so this is exact. In
// embedded mode (FX chain) all captures share one top-level, so is-active
// tells us "someone in this group is active"; we set g_active_capture to
// whichever capture the signal fired for and last-wins. That's fine because
// only one plugin is genuinely being interacted with at a time.
// Does this capture have an open modal? Modals are heavyweight -- a real
// dialog window the user is filling in -- so focus changes must NOT flip
// g_active_capture while one is up. Popups deliberately do NOT lock: clicking
// on another plugin while a menu is up is a legitimate "dismiss and switch"
// gesture, matching menu behaviour everywhere else. Menus close on focus loss
// on their own.
static bool has_live_ui(Capture *c)
{
    return c && !c->modals.empty();
}

static void on_toplevel_active(GObject *obj, GParamSpec *, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c) return;

    gboolean active = FALSE;
    g_object_get(obj, "is-active", &active, NULL);

    if (active) {
        // Don't steal active from a capture that is mid-interaction with the
        // user (menu up / modal open). Its own is-active is temporarily false
        // because the popup canvas or modal toplevel took focus; that's not a
        // genuine "user moved to a different plugin" event.
        if (g_active_capture && g_active_capture != c && has_live_ui(g_active_capture))
            return;

        g_active_capture = c;
        // Update cursor + raise on real activation only, not on hover noise.
        if (c->widget) {
            GdkWindow *gw = gtk_widget_get_window(c->widget);
            if (gw) {
                GdkCursor *cur = gdk_cursor_new_from_name(gdk_window_get_display(gw), "default");
                gdk_window_set_cursor(gw, cur);
                if (cur) g_object_unref(cur);
            }
        }
        if (c->dpy && c->parent_win) {
            XRaiseWindow(c->dpy, c->parent_win);
            XFlush(c->dpy);
        }
        xw_raise_modals();
    } else if (g_active_capture == c) {
        // Don't drop active while our own popup/modal is up -- the compositor
        // is just handing focus to the popup canvas / modal we spawned, not
        // telling us the user is done with this plugin.
        if (has_live_ui(c)) return;
        g_active_capture = nullptr;
    }
}

static void connect_widget(Capture *c)
{
    if (!c->widget) return;
    gtk_widget_add_events(c->widget,
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_SCROLL_MASK);

    // disable GTK's animation system entirely, globally -- the narrower
    // CSS-transition-property attempt didn't stop the repeated on_draw calls, so
    // testing whether animations are the cause at all, via a broader, more
    // definitive switch, before looking for a different mechanism entirely.
    {
        GtkSettings *settings = gtk_widget_get_settings(c->widget);
        if (settings) g_object_set(settings, "gtk-enable-animations", FALSE, NULL);
    }

    // Track compositor focus on the top-level -- this is the real "user is
    // interacting with this plugin" signal, not X11 EnterNotify.
    GtkWidget *toplevel = gtk_widget_get_toplevel(c->widget);
    if (toplevel && GTK_IS_WINDOW(toplevel)) {
        g_signal_connect(toplevel, "notify::is-active",
                         G_CALLBACK(on_toplevel_active), c);
        c->active_toplevel = toplevel;
        g_object_add_weak_pointer(G_OBJECT(toplevel),
                                  (gpointer*)&c->active_toplevel);
    }

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

    // Automatic composite redirection: the X server keeps drawing the plugin
    // window off-screen (it never has to be visible in root), and we sample
    // the resulting pixmap via XCompositeNameWindowPixmap. This is what the
    // pre-Xvfb bridge used, and it sidesteps the Wine input regression that
    // occurs when two Wine top-levels share the same X coordinates -- with
    // Automatic redirect the plugin windows never contest hit-testing at all.
    XCompositeRedirectWindow(dpy, plugin_win, CompositeRedirectAutomatic);

    // Cache the visual/depth once so on_draw does not have to XGetWindowAttributes
    // every frame.
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, plugin_win, &wa)) {
        c->visual = wa.visual;
        c->depth  = wa.depth;
        ensure_pixmap(c, wa.width, wa.height);
    }

    int base, err;
    if (g_wm_dpy && XDamageQueryExtension(g_wm_dpy, &base, &err)) {
        c->damage      = XDamageCreate(g_wm_dpy, plugin_win, XDamageReportBoundingBox);
        c->damage_base = base;
    }

    XFlush(dpy);

    register_capture(c);
    // A newly-created plugin is always the last one the user acted on -- opening
    // it is itself the "activate" action -- so treat it as the active capture
    // immediately. notify::is-active keeps this in sync afterwards; this just
    // avoids a race between plugin map and the compositor's focus decision.
    g_active_capture = c;
    DEBUG_PRINT("[XW] setup parent=0x%lx plugin=0x%lx (auto-composite, pixmap=0x%lx)\n",
                parent_win, plugin_win, (unsigned long)c->pixmap);
    return c;
}

static void cleanup_capture(Capture *c)
{
    if (!c) return;
    if (g_active_capture == c) g_active_capture = nullptr;
    // Disconnect notify::is-active BEFORE freeing the capture -- the toplevel
    // (REAPER's main window in embedded/FX-list mode) outlives us, and the
    // compositor routinely changes focus during an FX swap. Without this, the
    // signal fires with a dangling Capture* and gtk_widget_get_window(c->widget)
    // asserts.
    if (c->active_toplevel && GTK_IS_WIDGET(c->active_toplevel)) {
        g_signal_handlers_disconnect_by_data(c->active_toplevel, c);
        g_object_remove_weak_pointer(G_OBJECT(c->active_toplevel),
                                     (gpointer*)&c->active_toplevel);
        c->active_toplevel = nullptr;
    }
    unregister_capture(c);
    destroy_pixmap(c);
    Display *d = c->dpy;
    c->dpy = nullptr;
    if (d) XCloseDisplay(d);
    delete c;
}

static bool canvas_draw_cb(GtkWidget *, cairo_t *cr, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->dpy) return false;

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

        if (draw_x + p.w <= clip_x1 || draw_x >= clip_x2 ||
            draw_y + p.h <= clip_y1 || draw_y >= clip_y2)
            continue;

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

    GdkScreen *screen = gdk_screen_get_default();
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual != nullptr) {
        gtk_widget_set_visual(win, visual);
    }
    gtk_widget_set_app_paintable(win, TRUE);

    GtkWidget *top = c->widget ? gtk_widget_get_toplevel(c->widget) : nullptr;
    if (top && GTK_IS_WINDOW(top))
        gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(top));

    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);

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

static void refresh_gtk_offset(Capture *c, int *out_px = nullptr, int *out_py = nullptr)
{
    if (!c || !c->dpy || !c->widget) return;

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

    Capture::PopupWin *pp = nullptr;
    for (auto &e : c->popups) if (e.x11_win == x11_win) { pp = &e; break; }

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

    // Refresh the cached gtk offset before using it below. Without this,
    // c->gtk_x/y is only whatever xw_size last stored, which can be stale
    // when the plugin toplevel has moved (e.g. user drags a floating FX
    // window) between size events. Removing this call while removing the
    // slot code was a real bug -- side effect matters more than the out
    // params ever did.
    refresh_gtk_offset(c);

    if (!c->popup_canvas) create_popup_canvas(c);

    XCompositeRedirectWindow(c->dpy, x11_win, CompositeRedirectAutomatic);
    XFlush(c->dpy);
    Pixmap pixmap = XCompositeNameWindowPixmap(c->dpy, x11_win);

    if (pp) {
        if (pp->pixmap != None && pp->pixmap != pixmap) XFreePixmap(c->dpy, pp->pixmap);
    } else {
        c->popups.emplace_back();
        pp = &c->popups.back();
        if (g_wm_dpy && g_damage_event_base >= 0)
            pp->damage = XDamageCreate(g_wm_dpy, x11_win, XDamageReportBoundingBox);
    }
    pp->x11_win = x11_win;
    pp->pixmap  = pixmap;
    {
        XWindowAttributes wa_local;
        pp->visual = XGetWindowAttributes(c->dpy, x11_win, &wa_local)
                   ? wa_local.visual : DefaultVisual(c->dpy, DefaultScreen(c->dpy));
    }
    pp->x = attr->x + c->gtk_x;
    pp->y = attr->y + c->gtk_y;
    DEBUG_PRINT("[POPOFF] win=0x%lx attr=(%d,%d) gtk=(%d,%d) -> pp=(%d,%d)\n",
                (unsigned long)x11_win, attr->x, attr->y,
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
    md.w = attr->width; md.h = attr->height;
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

    for (auto &p : state->popups)
        if (p.x11_win == win) { canvas_add_popup(state, win, attr); return; }
    for (auto &m : state->modals)
        if (m.x11_win == win) return;

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

    // Match the newly-mapped window to its owning capture.
    //
    // Only the ACTIVE plugin can spawn popups. The user has to interact with a
    // plugin for it to open a menu / dialog, and "interact" implies the
    // compositor gave that plugin focus, which is what g_active_capture tracks.
    // Preferring it over PID first solves the multi-instance case: two copies
    // of the same plugin under one Wine host share a PID, so PID-only matching
    // would misattribute the popup to whichever capture the map iterates over
    // first.
    //
    // Still verify the PID relationship: if the active capture's PID doesn't
    // own this window, this popup is unrelated to it and we fall through to
    // the PID loop as a safety net (e.g. popups that appear without a prior
    // focus event).
    if (g_active_capture &&
        is_window_from_owned_plugin(g_active_capture->dpy, w, g_active_capture->gui_win)) {
        handle_new_window(w, g_active_capture, &attr);
        return;
    }

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

static bool on_popup_configured(Window w, int cx, int cy, int cw, int ch)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        for (auto &p : c->popups) {
            if (p.x11_win != w) continue;
            int ox = 0, oy = 0;
            refresh_gtk_offset(c, &ox, &oy);
            int old_x = p.x, old_y = p.y, old_w = p.w, old_h = p.h;
            {
                int fx = cx, fy = cy;
                if (fx < ox) fx += ox;
                if (fy < oy) fy += oy;
                p.x = fx + c->gtk_x;
                p.y = fy + c->gtk_y;
            }
            bool size_changed = (p.w != cw || p.h != ch);
            p.w = cw; p.h = ch;
            if (size_changed) {
                if (p.pixmap != None) XFreePixmap(c->dpy, p.pixmap);
                p.pixmap = XCompositeNameWindowPixmap(c->dpy, w);
            }
            if (c->canvas_draw) {
                GdkWindow *cwin = gtk_widget_get_window(c->canvas_draw);
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

static void handle_damage_notify(XDamageNotifyEvent *de)
{
    DEBUG_PRINT("damage: %dx%d+%d+%d\n",
        de->area.width, de->area.height, de->area.x, de->area.y);

    XDamageSubtract(g_wm_dpy, de->damage, None, None);

    // With Automatic redirection there is no capture step: the X server keeps
    // the composite pixmap in sync. All we need to do on damage is (a) refresh
    // the pixmap handle if the window has been resized (the handle becomes
    // invalid on every resize), and (b) tell GTK to redraw the affected area.
    auto refresh_pixmap_if_resized = [](Capture *c) -> bool {
        if (!c || !c->dpy || !c->plugin_win) return false;
        XWindowAttributes wa;
        if (!XGetWindowAttributes(c->dpy, c->plugin_win, &wa)) return false;
        if (wa.width <= 0 || wa.height <= 0) return false;
        return ensure_pixmap(c, wa.width, wa.height);
    };

    Capture *c = find_capture(de->drawable);
    if (c && c->widget && GTK_IS_WIDGET(c->widget)) {
        if (refresh_pixmap_if_resized(c)) {
            gtk_widget_queue_draw_area(c->widget, de->area.x, de->area.y,
                                       de->area.width, de->area.height);
        }
        return;
    }

    for (auto &kv : g_captures) {
        Capture *cc = kv.second;
        if (!cc) continue;

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
                    // Refresh the modal's OWN pixmap if the modal window was
                    // resized (composite pixmap becomes invalid on every
                    // resize). Then just tell GTK to redraw the damaged area
                    // -- with Automatic redirection the pixmap stays live on
                    // its own; no explicit capture step needed.
                    XWindowAttributes wa;
                    if (XGetWindowAttributes(cc->dpy, md.x11_win, &wa)
                        && wa.width > 0 && wa.height > 0) {
                        if (md.w != wa.width || md.h != wa.height || md.pixmap == 0) {
                            if (md.pixmap) XFreePixmap(cc->dpy, md.pixmap);
                            XFlush(cc->dpy);
                            md.pixmap = XCompositeNameWindowPixmap(cc->dpy, md.x11_win);
                            md.w = wa.width;
                            md.h = wa.height;
                        }
                    }
                    gtk_widget_queue_draw_area(md.draw, de->area.x, de->area.y,
                                               de->area.width, de->area.height);
                }
                return;
            }
        }
    }
}

static void handle_xdnd_from_plugin(XClientMessageEvent *xce)
{
    Display *dpy   = xce->display;
    const Atom mt  = xce->message_type;
    const Window self = xce->window;
    const Window src  = (Window)xce->data.l[0];

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
}

static void bridge_handle_event(XEvent *ev)
{
    Window dnd_catcher = g_wm ? g_wm->dnd_catcher_window() : 0;
    if (dnd_catcher &&
        ((ev->type == MapNotify   && ev->xmap.window   == dnd_catcher) ||
         (ev->type == UnmapNotify && ev->xunmap.window == dnd_catcher) ||
         (ev->type == ClientMessage && ev->xclient.window == dnd_catcher)))
        return;

    if (g_damage_event_base >= 0 && ev->type == g_damage_event_base + XDamageNotify) {
        handle_damage_notify((XDamageNotifyEvent *)ev);
        return;
    }

    if (ev->type == ClientMessage)
    {
        handle_xdnd_from_plugin(&ev->xclient);
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
        if (!c->modals.empty()){
            xw_raise_modals();
            any = true;
        }
        if (!c->popups.empty()){
            dismiss_popups_by_click(c);
            any = true;
        }
    }
    return any;
}

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
    return any;
}

void init_private_xwayland()
{
    if (!XWaylandWM::init_bridge_wm(":10")) return;

    if (g_wm_dpy) {
        int base, err;
        if (XDamageQueryExtension(g_wm_dpy, &base, &err)) {
            g_damage_event_base = base;
            g_bridge_damage_error_base = err;
        }
    }

    g_wm->on_unhandled_event = bridge_handle_event;
    g_wm->dnd_init();
}

// Recursively ensures h has its own real, positioned GtkFixed widget, creating one
// (and recursively ensuring its own parent has one first) if it doesn't already.
// A true toplevel (h->m_oswidget already set by SWELL itself) is the recursion's
// base case -- we never touch m_oswidget, only ever read it. Returns the widget to
// embed into, or nullptr if something in the chain isn't embeddable.
static GtkWidget* xw_ensure_embed_widget(HWND h)
{
    if (!h) return nullptr;

    if (h->m_oswidget) {
        // Real SWELL toplevel. Get (or create) its inner GtkFixed content area,
        // same as the existing container-unwrapping logic in xw_size.
        GtkWidget *top = (GtkWidget*)h->m_oswidget;
        if (!GTK_IS_WINDOW(top)) return top; // already a plain container, not a toplevel wrapper
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(top));
        if (child) return child;
        GtkWidget *fixed = gtk_fixed_new();
        gtk_container_add(GTK_CONTAINER(top), fixed);
        gtk_widget_show(fixed);
        return fixed;
    }

    int w = h->m_position.right - h->m_position.left;
    int hh = h->m_position.bottom - h->m_position.top;
    if (w < 1) w = 1;
    if (hh < 1) hh = 1;

    GtkWidget *parent_widget = xw_ensure_embed_widget(h->m_parent);
    if (!parent_widget || !GTK_IS_FIXED(parent_widget)) return nullptr;

    GtkWidget *existing = (GtkWidget*)GetProp(h, "XBridgeEmbedFixed");

    if (existing && GTK_IS_WIDGET(existing) && gtk_widget_get_parent(existing) != parent_widget) {
        existing = nullptr;
    }

    if (existing && GTK_IS_WIDGET(existing)) {
        gtk_fixed_move(GTK_FIXED(parent_widget), existing, h->m_position.left, h->m_position.top);
        gtk_widget_set_size_request(existing, w, hh);
        return existing;
    }

    GtkWidget *prev_fixed = (GtkWidget*)g_object_get_data(G_OBJECT(parent_widget), "xw-active-embed-fixed");
    if (prev_fixed && GTK_IS_WIDGET(prev_fixed) && prev_fixed != existing) {
        gtk_widget_destroy(prev_fixed);
    }

    GtkWidget *fixed = gtk_fixed_new();
    gtk_widget_set_size_request(fixed, w, hh);
    gtk_fixed_put(GTK_FIXED(parent_widget), fixed, h->m_position.left, h->m_position.top);
    gtk_widget_show(fixed);

    g_object_set_data(G_OBJECT(parent_widget), "xw-active-embed-fixed", fixed);
    SetProp(h, "XBridgeEmbedFixed", (HANDLE)fixed);
    return fixed;
}

static bool try_create_plugin(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return true;
    bridgeState *bs = (bridgeState*)hwnd->m_private_data;

    if (!bs->placed) {
        bs->embed_container = xw_ensure_embed_widget(hwnd->m_parent);
        xw_size(hwnd);
    }

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

    if (bs->placed && bs->cap) {
        KillTimer(hwnd, 1);
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
 
    // Has REAPER replaced the host toplevel underneath us?
    //
    // Same check SWELL's X11 bridge does in xbridgeProc's WM_SIZE/WM_MOVE
    // (swell-generic-gdk.cpp):
    //
    //     if (h && h->m_oswindow != bs->cur_parent) bs->need_reparent = true;
    //     ... gdk_window_reparent(bs->w, h->m_oswindow, ...);
    //         bs->cur_parent = h->m_oswindow;
    //
    // REAPER destroys and recreates the FX window when a plugin resizes itself
    // (Kontakt loading a library). swell_oswindow_destroy() destroys the
    // GdkWindow and nulls m_oswidget, then swell_oswindow_manage() builds a
    // fresh GtkWindow. Our drawing area stays in the discarded one: GTK still
    // reports it parented with a GtkWindow toplevel, but at the GDK level its
    // parent is NULL and the old toplevel's window reads back freed memory, so
    // it is never drawn again -- a live pixmap full of plugin content with
    // nothing painting it, recoverable only by reopening the plugin.
    //
    // Doing this here rather than from the damage callback matters: xw_size
    // runs on WM_SIZE/WM_MOVE, after SWELL has finished rebuilding the window,
    // which is exactly where the X11 bridge does it. Attempting the same from
    // a damage callback ran mid-teardown and segfaulted.
    {
        // Re-resolve the embed container every time and re-place if it moved.
        // Fixed float/unfloat in fx list view
        GtkWidget *fresh = xw_ensure_embed_widget(hwnd->m_parent);
        if (fresh && fresh != bs->embed_container) {
            DEBUG_PRINT("[REPARENT] embed container %p -> %p, re-placing\n",
                        (void*)bs->embed_container, (void*)fresh);
            bs->embed_container = fresh;
            bs->placed = false;   // the put/show path below re-places it
        }
        HWND top = hwnd->m_parent;
        while (top && !top->m_oswidget) top = top->m_parent;
        if (top && top->m_oswidget) bs->cur_parent_toplevel = (GtkWidget*)top->m_oswidget;
    }

    GtkWidget *container = bs->embed_container;
    if (!container || !GTK_IS_FIXED(container)) return;

    RECT r = hwnd->m_position;
    int pos_x = r.left, pos_y = r.top;
    int w = r.right - r.left, h = r.bottom - r.top;

    // title is bigger if compositor needs CSD
    if (g_swell_wayland_title_h > 0){
        pos_y += g_swell_wayland_title_h + (SWELL_WAYLAND_BORDER_WIDTH * 2);
        pos_x += SWELL_WAYLAND_BORDER_WIDTH * 2;
        w += SWELL_WAYLAND_BORDER_WIDTH * 2;
        h += SWELL_WAYLAND_BORDER_WIDTH * 2;
    }

    HWND top = hwnd->m_parent;
    while (top && !top->m_oswidget) top = top->m_parent;
    if (top && top->m_menu) {
        pos_y += GetSystemMetrics(SM_CYMENU);
    }

    bool size_changed = !bs->has_last_size_pos ||
        r.left != bs->last_size_pos.left || r.top != bs->last_size_pos.top ||
        r.right != bs->last_size_pos.right || r.bottom != bs->last_size_pos.bottom;

    // Re-capture the pixmap at the new size -- xw_size runs exactly when the
    // window is being resized, so the backing pixmap must be refreshed here.
    // Refresh BEFORE XResizeWindow, matching pre_xvfb's ordering: refresh_pixmap
    // flushes and re-names, so we snapshot the current server-side state, then
    // the resize proceeds; the next Damage event will bring the next refresh.
    // Guard on bs->cap: WM_SIZE/SetWindowPos can fire before the plugin is
    // captured (e.g. during an FX-list swap), and the container resize also
    // needs a live capture.
    if (bs->cap && size_changed) {
        refresh_pixmap(bs->cap);
        XResizeWindow(bs->cap->dpy, bs->cap->parent_win, w, h);
        XFlush(bs->cap->dpy);
    }

    if (!bs->placed) {
        // If we are re-placing after the host toplevel was replaced, the widget
        // may still be parented to the old (now dead) container. gtk_fixed_put()
        // refuses an already-parented widget, so detach it first, holding a
        // reference across the move so it is not finalized in between.
        {
            GtkWidget *oldp = gtk_widget_get_parent((GtkWidget*)hwnd->m_oswidget);
            if (oldp && oldp != container && GTK_IS_CONTAINER(oldp)) {
                g_object_ref(hwnd->m_oswidget);
                gtk_container_remove(GTK_CONTAINER(oldp), (GtkWidget*)hwnd->m_oswidget);
                gtk_fixed_put(GTK_FIXED(container), (GtkWidget*)hwnd->m_oswidget, pos_x, pos_y);
                g_object_unref(hwnd->m_oswidget);
                gtk_widget_set_size_request((GtkWidget*)hwnd->m_oswidget, w, h);
                gtk_widget_show((GtkWidget*)hwnd->m_oswidget);
                bs->placed = true;
                GtkWidget *tl2 = gtk_widget_get_toplevel((GtkWidget*)hwnd->m_oswidget);
                if (!(tl2 && GTK_IS_WINDOW(tl2))) bs->placed = false;
                if (size_changed) { bs->last_size_pos = r; bs->has_last_size_pos = true; }
                if (bs->cap) refresh_gtk_offset(bs->cap);
                return;
            }
        }
        gtk_fixed_put(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
        gtk_widget_set_size_request(hwnd->m_oswidget, w, h);
        gtk_widget_show(hwnd->m_oswidget);
        bs->placed = true;

        GtkWidget *tl = gtk_widget_get_toplevel(hwnd->m_oswidget);
        if (!(tl && GTK_IS_WINDOW(tl))) bs->placed = false;
    } else {
        gtk_fixed_move(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
        gtk_widget_set_size_request(hwnd->m_oswidget, w, h);
    }

    if (size_changed) {
        bs->last_size_pos = r;
        bs->has_last_size_pos = true;
    }

    // Refresh the popup offset so popups position correctly in both floating and
    // FX-list embedded modes. Must run after the widget's own repositioning above
    // (gtk_fixed_move/put), not before -- it reads the widget's actual on-screen
    // position, so computing it before the move used stale, pre-resize data. This
    // is what broke popup positioning specifically during a resize (e.g. Virtual Mix
    // Rack expanding the rack to make room for a dragged-in module).
    if (bs->cap) refresh_gtk_offset(bs->cap);
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

    g_object_add_weak_pointer(G_OBJECT(draw_area), (gpointer*)&hwnd->m_oswidget);

    bridgeState *bs = new bridgeState();
    bs->disp   = disp;
    bs->parent = container;
    hwnd->m_private_data = (INT_PTR)bs;

    *wref = (void*)container;

    {
        HWND top = viewpar;
        while (top && !top->m_oswidget) top = top->m_parent;
        if (top) SetProp(top, "SWELL_XW_BRIDGE_PLUGIN", (HANDLE)(INT_PTR)1);
    }

    xw_size(hwnd);

    SetTimer(hwnd, 1, 100, NULL);
    return hwnd;
}

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
