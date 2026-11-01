#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif
#include <gdk/gdkwayland.h>
#include <list>
#include "xwayland-bridge.h"
#include "xwayland-bridge-gl.h"
#include "swell-gdk-scale.h"
#include <xcb/xcb_renderutil.h>
#include "swell-xcb.h"
#include <xcb/composite.h>
#include <xcb/xtest.h>
#include <xcb/xfixes.h>
#include <xcb/randr.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/damage.h>
#include <xcb/shape.h>
#include <stdint.h>
#include <unistd.h>
void xw_size(HWND hwnd);


struct Capture {
    CapturePixmap pixmap;
    CaptureImage image;
    CaptureGL gl;
    cairo_device_t *cairo_device = nullptr;
    xcb_connection_t *connection = nullptr;
    xcb_window_t parent_win  = 0;
    xcb_window_t plugin_win  = 0;
    xcb_window_t gui_win     = 0;
    bool       pixmap_dirty = true;
    GtkWidget *widget      = nullptr;
    GtkWidget *active_toplevel = nullptr; // toplevel we hooked notify::is-active on; disconnected in cleanup
    HWND       hwnd        = nullptr;
    //
    xcb_damage_damage_t damage      = 0;

    GtkWidget *popup_canvas    = nullptr;
    GtkWidget *canvas_draw     = nullptr;
    GdkRectangle monitor_bounds = {};
    int        canvas_x        = 0;
    int        canvas_y        = 0;
    int        gtk_x           = 0;
    int        gtk_y           = 0;
    xcb_window_t root_popup      = 0;


    struct PopupWin {
        xcb_window_t x11_win;
        CapturePixmap pixmap;
        CaptureImage image;
        bool pixmap_dirty;
        int x,y,w,h;
        bool visible;
        xcb_damage_damage_t damage;
        std::vector<xcb_rectangle_t> shape;
        bool shape_dirty;
    };
    std::list<PopupWin> popups;

    // ── Modals (real dialog windows the plugin opens) ──
    struct ModalWin {
        xcb_window_t x11_win;
        CapturePixmap pixmap;
        CaptureImage image;
        bool pixmap_dirty;
        GtkWidget *gtk_win, *draw;
        xcb_damage_damage_t damage;
    };
    std::list<ModalWin> modals;
};

static std::map<xcb_window_t, Capture*> g_captures;

bool xw_bridge_has_pending_draw()
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (kv.first == c->parent_win && c->widget && c->pixmap_dirty &&
            gtk_widget_get_mapped(c->widget) &&
            !(gdk_window_get_state(gtk_widget_get_window(gtk_widget_get_toplevel(c->widget))) &
              GDK_WINDOW_STATE_ICONIFIED)) return true;
    }
    return false;
}

void xw_clip_capture_regions(GdkWindow *window, cairo_region_t *region)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (kv.first != c->parent_win || !c->widget || !c->pixmap.pixmap ||
            !gtk_widget_get_mapped(c->widget) || !gtk_widget_get_has_window(c->widget) ||
            gtk_widget_get_opacity(c->widget) != 1.0) continue;
        GtkWidget *top = gtk_widget_get_toplevel(c->widget);
        if (gtk_widget_get_window(top) != window) continue;
        if (c->pixmap.format.type != XCB_RENDER_PICT_TYPE_DIRECT || c->pixmap.format.direct.alpha_mask) continue;
        int x, y;
        if (!gtk_widget_translate_coordinates(c->widget, top, 0, 0, &x, &y)) continue;
        const int scale = swell_gdk_widget_scale(c->widget);
        cairo_rectangle_int_t rect = { 0, 0,
            min(gtk_widget_get_allocated_width(c->widget), c->pixmap.width / scale),
            min(gtk_widget_get_allocated_height(c->widget), c->pixmap.height / scale) };
        cairo_region_t *visible = gdk_window_get_visible_region(gtk_widget_get_window(c->widget));
        cairo_region_intersect_rectangle(visible, &rect);
        cairo_region_translate(visible, x, y);
        cairo_region_subtract(region, visible);
        cairo_region_destroy(visible);
    }
}

static int g_damage_event_base = -1;
static int g_shape_event_base = -1;
static xcb_connection_t *wm_connection()
{
    return g_wm ? g_wm->connection() : nullptr;
}

static xcb_damage_damage_t create_damage(xcb_window_t window)
{
    if (g_damage_event_base < 0) return 0;
    xcb_damage_damage_t damage = xcb_generate_id(wm_connection());
    xcb_damage_create(wm_connection(), damage, window, XCB_DAMAGE_REPORT_LEVEL_BOUNDING_BOX);
    return damage;
}

static void fake_motion(xcb_connection_t *connection, int x, int y)
{
    xcb_test_fake_input(connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME,
                        swell_xcb_screen(connection)->root, x, y, 0);
}

static void fake_button(xcb_connection_t *connection, int button, bool press)
{
    xcb_test_fake_input(connection, press ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE,
                        button, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
}

static void forward_key(xcb_connection_t *connection, xcb_window_t window, int keycode, int state, bool press)
{
    xcb_key_press_event_t event = {};
    event.response_type = press ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
    event.event = window;
    event.root = swell_xcb_screen(connection)->root;
    event.detail = keycode;
    event.state = state;
    event.same_screen = true;
    xcb_send_event(connection, true, window, press ? XCB_EVENT_MASK_KEY_PRESS : XCB_EVENT_MASK_KEY_RELEASE,
                    (const char*)&event);
    xcb_flush(connection);
}

static xcb_window_t first_child(xcb_connection_t *connection, xcb_window_t window)
{
    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, window), nullptr);
    xcb_window_t child = tree && xcb_query_tree_children_length(tree) ? xcb_query_tree_children(tree)[0] : XCB_NONE;
    free(tree);
    return child;
}

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
static Capture* find_capture(xcb_window_t w)
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

static void set_active_capture(Capture *c)
{
    g_active_capture = c;
    if (g_wm) g_wm->set_active_window(c ? c->parent_win : XCB_NONE);
}


struct bridgeState {
    xcb_connection_t *connection = nullptr;   // this plugin's X connection
    xcb_window_t parent = 0;         // container window in Xwayland
    Capture *cap    = nullptr;
    bool     placed = false;     // has the SWELL widget been put in its container
    RECT     last_size_pos = {0,0,0,0}; // last hwnd->m_position xw_size actually acted on, see xw_size
    bool     has_last_size_pos = false;
    GtkWidget *embed_container = nullptr; // resolved via xw_ensure_embed_widget, see xw_bridge_create/xw_size
};

static bool update_pixmap(Capture *c)
{
    if (!c || !c->connection || !c->plugin_win) return false;

    if (!c->pixmap.update(c->connection, c->plugin_win)) return false;
    c->pixmap_dirty = false;
    return true;
}

static void refresh_pixmap(Capture *c)
{
    if (!c) return;
    c->pixmap_dirty = true;
    if (c->widget)
        gtk_widget_queue_draw(c->widget);
}

static cairo_surface_t *read_pixels(Capture *c, CapturePixmap &pixmap, CaptureImage &image, bool refresh)
{
    cairo_surface_t *surface = image.read(pixmap, refresh);
    if (!surface) {
        surface = pixmap.surface();
        if (!c->cairo_device) c->cairo_device = cairo_device_reference(cairo_surface_get_device(surface));
    }
    return surface;
}

static bool on_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget))) return FALSE;
    const bool refresh = c && c->pixmap_dirty;
    if (c && c->pixmap_dirty) {
        const int old_w = c->pixmap.width, old_h = c->pixmap.height;
        if (update_pixmap(c) && (old_w != c->pixmap.width || old_h != c->pixmap.height))
            gtk_widget_queue_draw(widget);
    }
    if (!c || !c->pixmap.pixmap) return FALSE;

    cairo_surface_t *surf = read_pixels(c, c->pixmap, c->image, refresh);
    if (surf) {
        if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
            const int scale = swell_gdk_widget_scale(widget);
            cairo_save(cr);
            cairo_scale(cr, 1.0 / scale, 1.0 / scale);
            cairo_set_source_surface(cr, surf, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        }
        cairo_surface_destroy(surf);
    }
    return TRUE;
}

static Capture *find_capture(GdkWindow *window)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (kv.first == c->parent_win && c->widget && gtk_widget_get_window(c->widget) == window) return c;
    }
    return nullptr;
}

bool xw_bridge_draw(GdkWindow *window)
{
    Capture *c = find_capture(window);
    if (!c || !c->gl.enabled()) return false;
    if (c->pixmap_dirty) update_pixmap(c);
    if (!c->pixmap.pixmap) return false;
    if (c->gl.draw(c->widget, c->pixmap)) return true;
    c->pixmap_dirty = true;
    return false;
}

static xcb_window_t s_pointer_window;
static bool s_pointer_parked;

static void park_pointer(int x, int y)
{
    if (!wm_connection()) return;
    if (!s_pointer_window) {
        const uint32_t values[] = { true, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE };
        s_pointer_window = xcb_generate_id(wm_connection());
        xcb_create_window(wm_connection(), XCB_COPY_FROM_PARENT, s_pointer_window,
            swell_xcb_screen(wm_connection())->root, x, y, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
            XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
    }
    const uint32_t position[] = { (uint32_t)x, (uint32_t)y };
    xcb_configure_window(wm_connection(), s_pointer_window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, position);
    swell_xcb_raise(wm_connection(), s_pointer_window);
    xcb_map_window(wm_connection(), s_pointer_window);
    fake_motion(wm_connection(), x, y);
    swell_xcb_sync(wm_connection());
    s_pointer_parked = true;
}

static void resume_pointer()
{
    if (!s_pointer_parked) return;
    xcb_unmap_window(wm_connection(), s_pointer_window);
    swell_xcb_sync(wm_connection());
    s_pointer_parked = false;
}

static void forward_motion(Capture *c, double wx, double wy)
{
    resume_pointer();
    swell_xcb_raise(wm_connection(), c->parent_win);
    const int scale = swell_gdk_widget_scale(c->widget);
    int rx = 0, ry = 0;
    swell_xcb_translate(wm_connection(), c->plugin_win, swell_xcb_screen(c->connection)->root,
                          (int)(wx * scale), (int)(wy * scale), &rx, &ry);
    fake_motion(c->connection, rx, ry);
}

static gboolean on_crossing(GtkWidget *, GdkEventCrossing *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->connection || e->mode != GDK_CROSSING_NORMAL ||
        e->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    if (e->type == GDK_ENTER_NOTIFY) {
        forward_motion(c, e->x, e->y);
        xcb_flush(c->connection);
    } else if (!(e->state & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK |
                            GDK_BUTTON4_MASK | GDK_BUTTON5_MASK)) && c->popups.empty()) {
        xcb_motion_notify_event_t motion = {};
        motion.response_type = XCB_MOTION_NOTIFY;
        motion.event = c->gui_win ? c->gui_win : c->plugin_win;
        motion.root = swell_xcb_screen(c->connection)->root;
        motion.state = e->state;
        motion.same_screen = true;
        int x, y;
        if (swell_xcb_translate(c->connection, motion.root, motion.event, 0, 0, &x, &y)) {
            motion.event_x = x;
            motion.event_y = y;
            xcb_send_event(c->connection, true, motion.event, XCB_EVENT_MASK_POINTER_MOTION, (const char*)&motion);
            swell_xcb_sync(c->connection);
        }
        park_pointer(0, 0);
    }
    return FALSE;
}

void xw_raise_modals()
{
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c || kv.first != c->parent_win) continue;
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
    if (!c || !c->connection) return false;
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
    set_active_capture(c);
    if (c->hwnd) SetFocus(c->hwnd);
    forward_motion(c, e->x, e->y);
    fake_button(c->connection, e->button, true);
    xcb_flush(c->connection);
    return true;
}

static bool on_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    DEBUG_PRINT("[DNDX] widget button RELEASE btn=%d\n", e->button);
    Capture *c = (Capture*)data;
    if (!c || !c->connection) return false;
    forward_motion(c, e->x, e->y);
    fake_button(c->connection, e->button, false);
    xcb_flush(c->connection);
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
            const xcb_window_t parent = cc ? cc->parent_win : XCB_NONE;
            const char *lst[1] = { path };
            SWELL_InitiateDragDropOfFileList(cc ? cc->hwnd : NULL, NULL, lst, 1, NULL);
            DEBUG_PRINT("[DNDX] native drag finished\n");

            const auto it = g_captures.find(parent);
            if (!cc || it == g_captures.end() || it->second != cc) return true;
            if (cc->connection) g_wm->dnd_release_source_button(cc->connection);
        }
    }

    Capture *c = (Capture*)data;
    if (!c || !c->connection) return false;
    forward_motion(c, e->x, e->y);
    xcb_flush(c->connection);
    return true;
}

static bool on_scroll(GtkWidget *, GdkEventScroll *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->connection) return false;
    unsigned int btn = 0;
    switch (e->direction) {
        case GDK_SCROLL_UP:    btn = 4; break;
        case GDK_SCROLL_DOWN:  btn = 5; break;
        case GDK_SCROLL_LEFT:  btn = 6; break;
        case GDK_SCROLL_RIGHT: btn = 7; break;
        default: return false;
    }
    forward_motion(c, e->x, e->y);
    fake_button(c->connection, btn, true);
    fake_button(c->connection, btn, false);
    xcb_flush(c->connection);
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

        set_active_capture(c);
        // Update cursor + raise on real activation only, not on hover noise.
        if (c->widget) {
            GdkWindow *gw = gtk_widget_get_window(c->widget);
            if (gw) {
                GdkCursor *cur = gdk_cursor_new_from_name(gdk_window_get_display(gw), "default");
                gdk_window_set_cursor(gw, cur);
                if (cur) g_object_unref(cur);
            }
        }
        if (c->connection && c->parent_win) {
            swell_xcb_raise(c->connection, c->parent_win);
            xcb_flush(c->connection);
        }
        xw_raise_modals();
    } else if (g_active_capture == c) {
        // Don't drop active while our own popup/modal is up -- the compositor
        // is just handing focus to the popup canvas / modal we spawned, not
        // telling us the user is done with this plugin.
        if (has_live_ui(c)) return;
        set_active_capture(nullptr);
    }
}

static void connect_toplevel(Capture *c)
{
    GtkWidget *toplevel = c->widget ? gtk_widget_get_toplevel(c->widget) : nullptr;
    if (toplevel == c->active_toplevel) return;
    if (c->active_toplevel) {
        g_signal_handlers_disconnect_by_data(c->active_toplevel, c);
        g_object_remove_weak_pointer(G_OBJECT(c->active_toplevel),
                                     (gpointer*)&c->active_toplevel);
        c->active_toplevel = nullptr;
    }
    if (toplevel && GTK_IS_WINDOW(toplevel)) {
        c->active_toplevel = toplevel;
        g_object_add_weak_pointer(G_OBJECT(toplevel), (gpointer*)&c->active_toplevel);
        g_signal_connect(toplevel, "notify::is-active", G_CALLBACK(on_toplevel_active), c);
        on_toplevel_active(G_OBJECT(toplevel), nullptr, c);
    }
}

static void on_realize(GtkWidget *widget, gpointer)
{
    gdk_window_set_event_compression(gtk_widget_get_window(widget), FALSE);
}

static void on_unrealize(GtkWidget *, gpointer data)
{
    ((Capture*)data)->gl.reset();
}

static void connect_widget(Capture *c)
{
    if (!c->widget) return;
    g_object_add_weak_pointer(G_OBJECT(c->widget), (gpointer*)&c->widget);
    gtk_widget_add_events(c->widget,
                          GDK_POINTER_MOTION_MASK |
                          GDK_ENTER_NOTIFY_MASK |
                          GDK_LEAVE_NOTIFY_MASK |
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

    connect_toplevel(c);

    g_signal_connect(c->widget, "realize", G_CALLBACK(on_realize), c);
    g_signal_connect(c->widget, "unrealize", G_CALLBACK(on_unrealize), c);
    if (gtk_widget_get_realized(c->widget)) on_realize(c->widget, c);
    g_signal_connect(c->widget, "draw",                 G_CALLBACK(on_draw),           c);
    g_signal_connect(c->widget, "button-press-event",   G_CALLBACK(on_button_press),   c);
    g_signal_connect(c->widget, "button-release-event", G_CALLBACK(on_button_release), c);
    g_signal_connect(c->widget, "motion-notify-event",  G_CALLBACK(on_motion),         c);
    g_signal_connect(c->widget, "enter-notify-event",   G_CALLBACK(on_crossing),       c);
    g_signal_connect(c->widget, "leave-notify-event",   G_CALLBACK(on_crossing),       c);
    g_signal_connect(c->widget, "scroll-event",         G_CALLBACK(on_scroll),         c);
    gtk_widget_queue_draw(c->widget);
}

static Capture* setup_capture(xcb_connection_t *connection, xcb_window_t parent_win, xcb_window_t plugin_win, HWND hwnd)
{
    Capture *c = new Capture();
    c->connection        = connection;
    c->parent_win = parent_win;
    c->plugin_win = plugin_win;
    c->hwnd       = hwnd;

    xcb_xfixes_change_save_set(connection, XCB_SET_MODE_INSERT, XCB_XFIXES_SAVE_SET_TARGET_ROOT, XCB_XFIXES_SAVE_SET_MAPPING_UNMAP, plugin_win);

    // Automatic composite redirection: the X server keeps drawing the plugin
    // window off-screen (it never has to be visible in root), and we sample
    // the resulting pixmap via CompositeNameWindowPixmap. This is what the
    // pre-Xvfb bridge used, and it sidesteps the Wine input regression that
    // occurs when two Wine top-levels share the same X coordinates -- with
    // Automatic redirect the plugin windows never contest hit-testing at all.
    xcb_composite_redirect_window(connection, plugin_win, XCB_COMPOSITE_REDIRECT_AUTOMATIC);

    // Also caches the visual so on_draw does not have to
    // GetWindowAttributes every frame.
    update_pixmap(c);

    c->damage = create_damage(plugin_win);

    xcb_flush(connection);

    register_capture(c);
    // A newly-created plugin is always the last one the user acted on -- opening
    // it is itself the "activate" action -- so treat it as the active capture
    // immediately. notify::is-active keeps this in sync afterwards; this just
    // avoids a race between plugin map and the compositor's focus decision.
    set_active_capture(c);
    // Raise it too: containers overlap in Xwayland and XTest input goes to whichever
    // is on top. notify::is-active is hooked only after the new toplevel already
    // has focus, so on_toplevel_active never raises it and input keeps landing on
    // the previous plugin.
    swell_xcb_raise(connection, parent_win);
    xcb_flush(connection);
    DEBUG_PRINT("[XW] setup parent=0x%lx plugin=0x%lx (auto-composite, pixmap=0x%lx)\n",
                (unsigned long)parent_win, (unsigned long)plugin_win, (unsigned long)c->pixmap.pixmap);
    return c;
}

static void canvas_remove_popup(Capture *c, xcb_window_t x11_win);
static void modal_remove(Capture *c, xcb_window_t win);

static void cleanup_capture(Capture *c)
{
    if (!c) return;
    if (g_active_capture == c) set_active_capture(nullptr);
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
    if (c->widget) {
        g_signal_handlers_disconnect_by_data(c->widget, c);
        g_object_remove_weak_pointer(G_OBJECT(c->widget), (gpointer*)&c->widget);
    }
    while (!c->popups.empty()) canvas_remove_popup(c, c->popups.back().x11_win);
    while (!c->modals.empty()) modal_remove(c, c->modals.back().x11_win);
    if (c->damage && wm_connection()) xcb_damage_destroy(wm_connection(), c->damage);
    if (wm_connection()) xcb_flush(wm_connection());
    c->pixmap.reset();
    c->gl.reset();
    c->image.reset();
    if (c->cairo_device) {
        cairo_device_finish(c->cairo_device);
        cairo_device_destroy(c->cairo_device);
    }
    delete c;
}

static bool canvas_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->connection) return false;

    const int scale = swell_gdk_widget_scale(widget);
    cairo_save(cr);
    cairo_scale(cr, 1.0 / scale, 1.0 / scale);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    double clip_x1, clip_y1, clip_x2, clip_y2;
    cairo_clip_extents(cr, &clip_x1, &clip_y1, &clip_x2, &clip_y2);

    for (auto &p : c->popups) {
        const bool refresh = p.pixmap_dirty;
        if (p.visible && p.pixmap_dirty) {
            const int old_w = p.w, old_h = p.h;
            if (p.pixmap.update(c->connection, p.x11_win)) {
                p.w = p.pixmap.width;
                p.h = p.pixmap.height;
                p.pixmap_dirty = false;
                if (old_w != p.w || old_h != p.h) {
                    p.shape_dirty = true;
                    gtk_widget_queue_draw(widget);
                }
            }
        }
        if (!p.visible || !p.pixmap.pixmap) continue;

        int draw_x = p.x - c->canvas_x;
        int draw_y = p.y - c->canvas_y;

        if (draw_x + p.w <= clip_x1 || draw_x >= clip_x2 ||
            draw_y + p.h <= clip_y1 || draw_y >= clip_y2)
            continue;

        cairo_surface_t *surf = read_pixels(c, p.pixmap, p.image, refresh);
        if (surf) {
            cairo_save(cr);
            cairo_rectangle(cr, draw_x, draw_y, p.w, p.h);
            cairo_clip(cr);
            if (g_shape_event_base >= 0) {
                if (p.shape_dirty) {
                    xcb_shape_get_rectangles_reply_t *reply = xcb_shape_get_rectangles_reply(wm_connection(),
                        xcb_shape_get_rectangles(wm_connection(), p.x11_win, XCB_SHAPE_SK_BOUNDING), nullptr);
                    p.shape.clear();
                    if (reply) {
                        const xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(reply);
                        p.shape.assign(rects, rects + xcb_shape_get_rectangles_rectangles_length(reply));
                        free(reply);
                    }
                    p.shape_dirty = false;
                }
                for (const auto &r : p.shape)
                    cairo_rectangle(cr, draw_x + r.x, draw_y + r.y, r.width, r.height);
                cairo_clip(cr);
            }
            cairo_set_source_surface(cr, surf, draw_x, draw_y);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_surface_destroy(surf);
        }
    }
    cairo_restore(cr);
    return true;
}

static bool popup_accepts_input(xcb_connection_t *connection, xcb_window_t window)
{
    xcb_shape_get_rectangles_reply_t *reply = xcb_shape_get_rectangles_reply(connection,
        xcb_shape_get_rectangles(connection, window, XCB_SHAPE_SK_INPUT), nullptr);
    const bool accepts = reply && xcb_shape_get_rectangles_rectangles_length(reply) > 0;
    free(reply);
    return accepts;
}

static void dismiss_popups(Capture *c);

static bool canvas_button_press(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->connection) return false;
    resume_pointer();

    const int scale = swell_gdk_widget_scale(widget);
    int screen_x = (int)(e->x * scale) + c->canvas_x;
    int screen_y = (int)(e->y * scale) + c->canvas_y;
    int x11_x = screen_x - c->gtk_x;
    int x11_y = screen_y - c->gtk_y;

    bool hit = false;
    for (auto it = c->popups.rbegin(); it != c->popups.rend(); ++it) {
        if (!it->visible) continue;
        if (screen_x >= it->x && screen_x < it->x + it->w &&
            screen_y >= it->y && screen_y < it->y + it->h &&
            popup_accepts_input(c->connection, it->x11_win)) {
            hit = true;
            fake_motion(c->connection, x11_x, x11_y);
            xcb_flush(c->connection);
            fake_button(c->connection, e->button, true);
            xcb_flush(c->connection);
            return true;
        }
    }
    // Clicked outside any popup — dismiss.
    if (!hit) {
        dismiss_popups(c);
        return true;
    }
    return false;
}

static bool canvas_button_release(GtkWidget *, GdkEventButton *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->connection) return false;
    fake_button(c->connection, e->button, false);
    xcb_flush(c->connection);
    return true;
}

static bool canvas_motion(GtkWidget *widget, GdkEventMotion *e, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c || !c->connection) return false;
    resume_pointer();

    static guint32 last_time = 0;
    guint32 now = g_get_monotonic_time() / 1000;
    if (now - last_time < 16) return true;
    last_time = now;

    const int scale = swell_gdk_widget_scale(widget);
    int screen_x = (int)(e->x * scale) + c->canvas_x;
    int screen_y = (int)(e->y * scale) + c->canvas_y;
    int x11_x = screen_x - c->gtk_x;
    int x11_y = screen_y - c->gtk_y;

    fake_motion(c->connection, x11_x, x11_y);
    xcb_flush(c->connection);

    return true;
}

static GdkRectangle capture_monitor(xcb_connection_t *connection, GtkWidget *widget)
{
    xcb_screen_t *screen = swell_xcb_screen(connection);
    GdkRectangle bounds = { 0, 0, screen->width_in_pixels, screen->height_in_pixels };
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection,
        xcb_get_geometry(connection, screen->root), nullptr);
    if (geometry) {
        bounds.width = geometry->width;
        bounds.height = geometry->height;
        free(geometry);
    }
    xcb_randr_get_monitors_reply_t *reply = xcb_randr_get_monitors_reply(connection,
        xcb_randr_get_monitors(connection, screen->root, true), nullptr);
    if (!reply) return bounds;

    GdkWindow *window = widget ? gtk_widget_get_window(widget) : nullptr;
    GdkMonitor *monitor = window ? gdk_display_get_monitor_at_window(
                                      gdk_window_get_display(window), window) : nullptr;
    GdkRectangle target = bounds;
    if (monitor) {
        gdk_monitor_get_geometry(monitor, &target);
        const int scale = gdk_monitor_get_scale_factor(monitor);
        target.width *= scale;
        target.height *= scale;
    }
    GdkRectangle selected = bounds;
    int best = -1;
    for (xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(reply);
         it.rem; xcb_randr_monitor_info_next(&it)) {
        GdkRectangle candidate = { it.data->x, it.data->y, it.data->width, it.data->height };
        if (!gdk_rectangle_intersect(&bounds, &candidate, &candidate)) continue;
        const int distance = abs(candidate.width - target.width) + abs(candidate.height - target.height);
        if (best < 0 || distance < best) {
            best = distance;
            selected = candidate;
        }
    }
    free(reply);
    return selected;
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

    const GdkRectangle bounds = c->monitor_bounds;
    const int scale = swell_gdk_widget_scale(c->widget);
    c->canvas_x = swell_gdk_floor(bounds.x + c->gtk_x, scale) * scale;
    c->canvas_y = swell_gdk_floor(bounds.y + c->gtk_y, scale) * scale;

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, swell_gdk_ceil(bounds.width, scale),
                                    swell_gdk_ceil(bounds.height, scale));
    gtk_container_add(GTK_CONTAINER(win), da);

    gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(da, "draw",                 G_CALLBACK(canvas_draw_cb),        c);
    g_signal_connect(da, "button-press-event",   G_CALLBACK(canvas_button_press),   c);
    g_signal_connect(da, "button-release-event", G_CALLBACK(canvas_button_release), c);
    g_signal_connect(da, "motion-notify-event",  G_CALLBACK(canvas_motion),         c);

    c->popup_canvas = win;
    c->canvas_draw  = da;
    gtk_widget_realize(win);
    GdkRectangle anchor = { 0, 0, 1, 1 };
    gdk_window_move_to_rect(gtk_widget_get_window(win), &anchor,
                           GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST,
                           (GdkAnchorHints)0, c->canvas_x / scale, c->canvas_y / scale);
}

static void refresh_gtk_offset(Capture *c, int *out_px = nullptr, int *out_py = nullptr)
{
    if (!c || !c->connection || !c->widget) return;

    int px = 0, py = 0;
    swell_xcb_translate(c->connection, c->parent_win,
                          swell_xcb_screen(c->connection)->root, 0, 0, &px, &py);
    if (out_px) *out_px = px;
    if (out_py) *out_py = py;

    GdkWindow *ww = gtk_widget_get_window(c->widget);
    if (!ww) return;

    int wox = 0, woy = 0;
    gdk_window_get_origin(ww, &wox, &woy);
    const int scale = swell_gdk_window_scale(ww);
    c->gtk_x = wox * scale - px;
    c->gtk_y = woy * scale - py;
}

static void canvas_add_popup(Capture *c, xcb_window_t x11_win, xcb_get_geometry_reply_t *attr)
{
    if (!c) return;

    // Refresh the cached gtk offset before using it below. Without this,
    // c->gtk_x/y is only whatever xw_size last stored, which can be stale
    // when the plugin toplevel has moved (e.g. user drags a floating FX
    // window) between size events. Removing this call while removing the
    // slot code was a real bug -- side effect matters more than the out
    // params ever did.
    refresh_gtk_offset(c);
    int x, y;
    if (!swell_xcb_translate(c->connection, x11_win, swell_xcb_screen(c->connection)->root,
                               0, 0, &x, &y)) return;
    x += c->gtk_x;
    y += c->gtk_y;

    Capture::PopupWin *pp = nullptr;
    for (auto &e : c->popups) if (e.x11_win == x11_win) { pp = &e; break; }

    if (pp && pp->pixmap.pixmap &&
        pp->x == x && pp->y == y &&
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

    if (!c->popup_canvas) create_popup_canvas(c);

    xcb_composite_redirect_window(c->connection, x11_win, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
    xcb_flush(c->connection);
    if (!pp) {
        c->popups.emplace_back();
        pp = &c->popups.back();
        if (wm_connection() && g_damage_event_base >= 0)
            pp->damage = create_damage(x11_win);
        if (wm_connection() && g_shape_event_base >= 0)
            xcb_shape_select_input(wm_connection(), x11_win, true);
    }
    pp->x11_win = x11_win;
    pp->pixmap_dirty = true;
    pp->x = x;
    pp->y = y;
    DEBUG_PRINT("[POPOFF] win=0x%lx attr=(%d,%d) gtk=(%d,%d) -> pp=(%d,%d)\n",
                (unsigned long)x11_win, attr->x, attr->y,
                c->gtk_x, c->gtk_y, pp->x, pp->y);
    pp->w = attr->width;
    pp->h = attr->height;
    pp->visible = true;
    pp->shape_dirty = true;

    if (c->root_popup == XCB_NONE && popup_accepts_input(c->connection, x11_win))
        c->root_popup = x11_win;

    if (!gtk_widget_get_visible(c->popup_canvas))
        gtk_widget_show_all(c->popup_canvas);
    if (c->canvas_draw) gtk_widget_queue_draw(c->canvas_draw);
}

static void canvas_remove_popup(Capture *c, xcb_window_t x11_win)
{
    if (!c) return;
    DEBUG_PRINT("[DNDCOORD] canvas_remove_popup win=0x%lx remaining_before=%zu\n",
                (unsigned long)x11_win, c->popups.size());
    for (auto it = c->popups.begin(); it != c->popups.end(); ) {
        if (it->x11_win == x11_win) {
            if (it->damage && wm_connection()) { xcb_damage_destroy(wm_connection(), it->damage); it->damage = 0; }
            it = c->popups.erase(it);
        } else ++it;
    }
    if (c->root_popup == x11_win) c->root_popup = XCB_NONE;

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

struct ModalRender { Capture *cap; xcb_window_t x11_win; };

static void modal_request_close(Capture *c, xcb_window_t window)
{
    xcb_client_message_event_t event = {};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = window;
    event.type = g_wm->atoms().WM_PROTOCOLS;
    event.format = 32;
    event.data.data32[0] = g_wm->atoms().WM_DELETE_WINDOW;
    event.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(c->connection, false, window, XCB_EVENT_MASK_NO_EVENT, (const char*)&event);
    xcb_flush(c->connection);
}

static gboolean modal_delete_cb(GtkWidget *, GdkEvent *, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    modal_request_close(m->cap, m->x11_win);
    return TRUE;
}

static bool modal_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap || !m->cap->connection) return false;
    Capture::ModalWin *modal = nullptr;
    for (auto &md : m->cap->modals)
        if (md.x11_win == m->x11_win) { modal = &md; break; }
    if (!modal) return false;
    const bool refresh = modal->pixmap_dirty;
    if (refresh && modal->pixmap.update(m->cap->connection, m->x11_win))
        modal->pixmap_dirty = false;
    if (!modal->pixmap.pixmap) return false;

    cairo_surface_t *surf = read_pixels(m->cap, modal->pixmap, modal->image, refresh);
    if (surf) {
        const int scale = swell_gdk_widget_scale(widget);
        cairo_save(cr);
        cairo_scale(cr, 1.0 / scale, 1.0 / scale);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(surf);
    }
    return true;
}

static void modal_forward_motion(ModalRender *m, GtkWidget *widget, double wx, double wy)
{
    resume_pointer();
    const int scale = swell_gdk_widget_scale(widget);
    int rx = 0, ry = 0;
    swell_xcb_translate(m->cap->connection, m->x11_win, swell_xcb_screen(m->cap->connection)->root,
                          (int)(wx * scale), (int)(wy * scale), &rx, &ry);
    fake_motion(m->cap->connection, rx, ry);
}

static bool modal_button_press(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->connection) return false;
    modal_forward_motion(m, widget, e->x, e->y);
    fake_button(m->cap->connection, e->button, true);
    xcb_flush(m->cap->connection);
    return true;
}

static bool modal_button_release(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->connection) return false;
    modal_forward_motion(m, widget, e->x, e->y);
    fake_button(m->cap->connection, e->button, false);
    xcb_flush(m->cap->connection);
    return true;
}

static bool modal_motion(GtkWidget *widget, GdkEventMotion *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->connection) return false;
    modal_forward_motion(m, widget, e->x, e->y);
    xcb_flush(m->cap->connection);
    return true;
}

static bool modal_key(GtkWidget *, GdkEventKey *e, gpointer data)
{
    ModalRender *m = (ModalRender*)data;
    if (!m || !m->cap->connection) return false;

    forward_key(m->cap->connection, m->x11_win, e->hardware_keycode, e->state, e->type == GDK_KEY_PRESS);
    return true;
}

static void modal_render_destroy(gpointer data, GClosure *) { delete (ModalRender*)data; }

static void create_modal(Capture *state, xcb_window_t win, xcb_get_geometry_reply_t *attr)
{
    // Never create a second modal for the same window.
    for (auto &m : state->modals) if (m.x11_win == win) return;

    xcb_connection_t *connection = state->connection;
    xcb_composite_redirect_window(connection, win, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
    xcb_flush(connection);

    GtkWidget *gtk_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(gtk_win), TRUE);

    GtkWidget *toplevel = state->widget ? gtk_widget_get_toplevel(state->widget) : nullptr;
    if (toplevel && GTK_IS_WINDOW(toplevel))
        gtk_window_set_transient_for(GTK_WINDOW(gtk_win), GTK_WINDOW(toplevel));

    gtk_window_set_type_hint(GTK_WINDOW(gtk_win), GDK_WINDOW_TYPE_HINT_DIALOG);

    const int scale = swell_gdk_widget_scale(state->widget);
    gtk_window_resize(GTK_WINDOW(gtk_win), swell_gdk_ceil(attr->width, scale), swell_gdk_ceil(attr->height, scale));

    GtkWidget *draw = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw, swell_gdk_ceil(attr->width, scale), swell_gdk_ceil(attr->height, scale));
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
    g_signal_connect(draw, "draw", G_CALLBACK(modal_draw_cb), mr);
    g_signal_connect_data(gtk_win, "delete-event", G_CALLBACK(modal_delete_cb), mr, modal_render_destroy, (GConnectFlags)0);

    gtk_widget_show_all(gtk_win);
    gtk_widget_grab_focus(draw);

    xcb_damage_damage_t dmg = 0;
    if (wm_connection() && g_damage_event_base >= 0)
        dmg = create_damage(win);

    state->modals.emplace_back();
    Capture::ModalWin &md = state->modals.back();
    md.x11_win = win; md.gtk_win = gtk_win; md.draw = draw; md.damage = dmg;
    md.pixmap_dirty = true;
}

static void modal_remove(Capture *c, xcb_window_t win)
{
    for (auto it = c->modals.begin(); it != c->modals.end(); ++it)
        if (it->x11_win == win) {
            xcb_damage_damage_t dmg = it->damage;
            GtkWidget *w   = it->gtk_win;
            c->modals.erase(it);
            if (dmg && wm_connection())       xcb_damage_destroy(wm_connection(), dmg);
            if (w && GTK_IS_WIDGET(w)) gtk_widget_destroy(w);
            break;
        }
}

static void handle_new_window(xcb_window_t win, Capture *state, xcb_get_geometry_reply_t *attr, bool override_redirect) {
    if (!state || !state->connection || !win) return;

    for (auto &p : state->popups)
        if (p.x11_win == win) { canvas_add_popup(state, win, attr); return; }
    for (auto &m : state->modals)
        if (m.x11_win == win) return;

    const XWaylandWM::WindowType type = g_wm->window_type(win, override_redirect);
    if (type == XWaylandWM::DRAG_ICON) {
        DEBUG_PRINT("[DNDCOORD] ignoring XDND icon window 0x%lx (not compositing as popup)\n", (unsigned long)win);
        return;
    }

    if (type == XWaylandWM::POPUP) {
        canvas_add_popup(state, win, attr);
    } else {
        create_modal(state, win, attr);
    }
}

static void on_popup_mapped(xcb_window_t w)
{
    xcb_get_window_attributes_cookie_t attributes_request = xcb_get_window_attributes(wm_connection(), w);
    xcb_get_geometry_cookie_t geometry_request = xcb_get_geometry(wm_connection(), w);
    xcb_get_window_attributes_reply_t *attributes = xcb_get_window_attributes_reply(wm_connection(), attributes_request, nullptr);
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(wm_connection(), geometry_request, nullptr);
    if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE || !geometry) {
        free(attributes);
        free(geometry);
        return;
    }
    const bool override_redirect = attributes->override_redirect;
    xcb_get_geometry_reply_t attr = *geometry;
    free(attributes);
    free(geometry);

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
        g_wm->owns_window(w, g_active_capture->gui_win)) {
        handle_new_window(w, g_active_capture, &attr, override_redirect);
        return;
    }

    for (auto &kv : g_captures) {
        Capture *cand = kv.second;
        if (!cand || kv.first != cand->parent_win) continue;
        if (cand && g_wm->owns_window(w, cand->gui_win)) {
            handle_new_window(w, cand, &attr, override_redirect);
            return;
        }
    }
}

static void on_popup_unmapped(xcb_window_t w)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (!c || kv.first != c->parent_win) continue;
        auto it = std::find_if(c->popups.begin(), c->popups.end(),
            [w](const Capture::PopupWin &p){ return p.x11_win == w; });
        if (it != c->popups.end()) { canvas_remove_popup(c, w); return; }

        auto mit = std::find_if(c->modals.begin(), c->modals.end(),
            [w](const Capture::ModalWin &m){ return m.x11_win == w; });
        if (mit != c->modals.end()) { modal_remove(c, w); return; }
    }
}

static bool on_popup_configured(xcb_window_t w, int cw, int ch)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (!c || kv.first != c->parent_win) continue;
        for (auto &p : c->popups) {
            if (p.x11_win != w) continue;
            int x, y;
            if (!swell_xcb_translate(c->connection, w, swell_xcb_screen(c->connection)->root,
                                       0, 0, &x, &y)) return true;
            refresh_gtk_offset(c);
            int old_x = p.x, old_y = p.y, old_w = p.w, old_h = p.h;
            p.x = x + c->gtk_x;
            p.y = y + c->gtk_y;
            bool size_changed = (p.w != cw || p.h != ch);
            p.w = cw; p.h = ch;
            if (size_changed) {
                p.shape_dirty = true;
                p.pixmap_dirty = true;
            }
            if (c->canvas_draw) {
                GdkWindow *cwin = gtk_widget_get_window(c->canvas_draw);
                if (cwin) {
                    GdkRectangle old_r = { old_x - c->canvas_x, old_y - c->canvas_y, old_w, old_h };
                    GdkRectangle new_r = { p.x - c->canvas_x, p.y - c->canvas_y, p.w, p.h };
                    const int scale = swell_gdk_window_scale(cwin);
                    old_r = swell_gdk_logical_rect(old_r, scale);
                    new_r = swell_gdk_logical_rect(new_r, scale);
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

static void handle_damage_notify(xcb_damage_notify_event_t *de)
{
    DEBUG_PRINT("damage: %dx%d+%d+%d\n",
        de->area.width, de->area.height, de->area.x, de->area.y);

    xcb_damage_subtract(wm_connection(), de->damage, XCB_NONE, XCB_NONE);

    Capture *c = find_capture(de->drawable);
    if (c && c->widget && GTK_IS_WIDGET(c->widget)) {
        c->pixmap_dirty = true;
        if (c->pixmap.width != de->geometry.width || c->pixmap.height != de->geometry.height)
            gtk_widget_queue_draw(c->widget);
        else
            swell_gdk_queue_draw_pixels(c->widget, de->area.x, de->area.y,
                                       de->area.width, de->area.height);
        return;
    }

    for (auto &kv : g_captures) {
        Capture *cc = kv.second;
        if (!cc || kv.first != cc->parent_win) continue;

        for (auto &pp : cc->popups) {
            if (pp.x11_win == de->drawable) {
                pp.pixmap_dirty = true;
                if (cc->canvas_draw && GTK_IS_WIDGET(cc->canvas_draw)) {
                    int inv_x = pp.x + de->area.x - cc->canvas_x;
                    int inv_y = pp.y + de->area.y - cc->canvas_y;
                    swell_gdk_queue_draw_pixels(cc->canvas_draw, inv_x, inv_y,
                                               de->area.width, de->area.height);
                }
                return;
            }
        }

        for (auto &md : cc->modals) {
            if (md.x11_win == de->drawable) {
                if (md.draw && GTK_IS_WIDGET(md.draw)) {
                    md.pixmap_dirty = true;
                    swell_gdk_queue_draw_pixels(md.draw, de->area.x, de->area.y,
                                               de->area.width, de->area.height);
                }
                return;
            }
        }
    }
}

static void handle_xdnd_from_plugin(xcb_client_message_event_t *event)
{
    const WMAtoms &atoms = g_wm->atoms();
    xcb_client_message_event_t reply = {};
    reply.response_type = XCB_CLIENT_MESSAGE;
    reply.window = event->data.data32[0];
    if (!reply.window) return;
    if (event->type == atoms.XdndPosition) reply.type = atoms.XdndStatus;
    else if (event->type == atoms.XdndDrop) reply.type = atoms.XdndFinished;
    else return;
    reply.format = 32;
    reply.data.data32[0] = event->window;
    xcb_send_event(wm_connection(), false, reply.window, XCB_EVENT_MASK_NO_EVENT, (const char*)&reply);
    xcb_flush(wm_connection());
}

static void bridge_handle_event(xcb_generic_event_t *ev)
{
    const uint8_t type = ev->response_type & 0x7f;
    xcb_window_t dnd_catcher = g_wm ? g_wm->dnd_catcher_window() : 0;
    if (dnd_catcher &&
        ((type == XCB_MAP_NOTIFY   && ((xcb_map_notify_event_t*)ev)->window   == dnd_catcher) ||
         (type == XCB_UNMAP_NOTIFY && ((xcb_unmap_notify_event_t*)ev)->window == dnd_catcher) ||
         (type == XCB_CLIENT_MESSAGE && ((xcb_client_message_event_t*)ev)->window == dnd_catcher)))
        return;

    if (g_damage_event_base >= 0 && type == g_damage_event_base + XCB_DAMAGE_NOTIFY) {
        handle_damage_notify((xcb_damage_notify_event_t *)ev);
        return;
    }

    if (g_shape_event_base >= 0 && type == g_shape_event_base + XCB_SHAPE_NOTIFY) {
        xcb_shape_notify_event_t *shape = (xcb_shape_notify_event_t *)ev;
        if (shape->shape_kind != XCB_SHAPE_SK_BOUNDING) return;
        for (auto &kv : g_captures) {
            Capture *c = kv.second;
            if (kv.first != c->parent_win) continue;
            for (auto &p : c->popups) {
                if (p.x11_win != shape->affected_window) continue;
                p.shape_dirty = true;
                if (c->canvas_draw)
                    swell_gdk_queue_draw_pixels(c->canvas_draw, p.x - c->canvas_x,
                                                p.y - c->canvas_y, p.w, p.h);
                return;
            }
        }
        return;
    }

    if (type == XCB_CLIENT_MESSAGE)
    {
        handle_xdnd_from_plugin((xcb_client_message_event_t*)ev);
        return;
    }

    // Non-damage: popup lifecycle. (c set => plugin window, nothing to do here.)
    if (type != XCB_MAP_NOTIFY && type != XCB_UNMAP_NOTIFY && type != XCB_CONFIGURE_NOTIFY) return;
    Capture *c = find_capture(((xcb_map_notify_event_t*)ev)->window);
    if (c) return;
    switch (type)
    {
        case XCB_MAP_NOTIFY:
            on_popup_mapped(((xcb_map_notify_event_t*)ev)->window);
            break;
        case XCB_UNMAP_NOTIFY:
            on_popup_unmapped(((xcb_unmap_notify_event_t*)ev)->window);
            break;
        case XCB_CONFIGURE_NOTIFY:
            on_popup_configured(((xcb_configure_notify_event_t*)ev)->window,
                                ((xcb_configure_notify_event_t*)ev)->width, ((xcb_configure_notify_event_t*)ev)->height);
            break;
        default:
            break;
    }
}

static void dismiss_popups(Capture *c)
{
    if (!c || !c->connection || c->popups.empty()) return;
    park_pointer(0, 0);
    if (c->root_popup) {
        int levels = 0;
        for (const auto &p : c->popups)
            if (p.visible && popup_accepts_input(c->connection, p.x11_win)) ++levels;
        xcb_key_symbols_t *symbols = xcb_key_symbols_alloc(c->connection);
        xcb_keycode_t *codes = symbols ? xcb_key_symbols_get_keycode(symbols, 0xff1b) : nullptr;
        const int keycode = codes ? codes[0] : 0;
        free(codes);
        if (symbols) xcb_key_symbols_free(symbols);
        if (!keycode) return;
        for (int i = 0; i < wdl_max(levels, 1); ++i) {
            xw_bridge_forward_key(c->hwnd, keycode, 0, true);
            xw_bridge_forward_key(c->hwnd, keycode, 0, false);
        }
    }
}

bool xw_bridge_swell_on_button_event_escape()
{
    bool any = false;
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c || kv.first != c->parent_win) continue;
        if (!c->modals.empty()){
            xw_raise_modals();
            any = true;
        }
        if (!c->popups.empty()){
            dismiss_popups(c);
            any = true;
        }
    }
    return any;
}

bool xw_bridge_swell_on_gdk_delete_release(HWND hwnd)
{
    bool any = false;
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (!c || kv.first != c->parent_win) continue;
        HWND owner = c->hwnd;
        while (owner && owner != hwnd) owner = owner->m_parent ? owner->m_parent : owner->m_owner;
        if (!owner) continue;
        if (!c->popups.empty()) {
            any = true;
            // Tell the plugin to close its menu FIRST, while we still have the popup
            // list -- dismiss_popups() returns immediately if popups is empty,
            // so doing this after the clear below (as the `if (any)` block used to) was
            // a no-op. Wine was left holding the menu's pointer grab as its window was
            // destroyed, which wedges the plugin and hangs REAPER. Modals are fine
            // because they take no pointer grab.
            dismiss_popups(c);
            while (!c->popups.empty()) canvas_remove_popup(c, c->popups.back().x11_win);
        }
        if (!c->modals.empty() && c->connection) {
            // Modals must be dismissed too. Previously this branch only *checked* for
            // an active modal, so Super+Q left the dialog open and, because the check
            // also gated `any`, the close did nothing at all -- leaving a plugin with
            // an open dialog that hangs the moment REAPER regains focus.
            //
            // A dialog cannot be dismissed the way a popup is: a synthetic click would
            // press whatever button has focus, and Escape needs X11 keyboard focus we
            // cannot guarantee. WM_DELETE_WINDOW is the protocol-correct "please
            // close" and Wine dialogs honour it regardless of focus.
            for (auto &md : c->modals) {
                if (!md.x11_win) continue;
                any = true;
                modal_request_close(c, md.x11_win);
            }
            xcb_flush(c->connection);
        }
    }
    return any;
}

void init_private_xwayland()
{
    if (!XWaylandWM::init_bridge_wm()) return;

    const xcb_query_extension_reply_t *damage = xcb_get_extension_data(wm_connection(), &xcb_damage_id);
    if (damage && damage->present) {
        free(xcb_damage_query_version_reply(wm_connection(), xcb_damage_query_version(wm_connection(), 1, 1), nullptr));
        g_damage_event_base = damage->first_event;
    }
    const xcb_query_extension_reply_t *shape = xcb_get_extension_data(wm_connection(), &xcb_shape_id);
    if (shape && shape->present) {
        free(xcb_shape_query_version_reply(wm_connection(), xcb_shape_query_version(wm_connection()), nullptr));
        g_shape_event_base = shape->first_event;
    }

    g_wm->on_unhandled_event = bridge_handle_event;
    g_wm->dnd_init();
}

struct XwFixed { GtkFixed parent; };
struct XwFixedClass { GtkFixedClass parent; };
G_DEFINE_TYPE(XwFixed, xw_fixed, GTK_TYPE_FIXED)

static void xw_fixed_get_preferred_size(GtkWidget *, gint *minimum, gint *natural)
{
    if (minimum) *minimum = 1;
    if (natural) *natural = 1;
}

static void xw_fixed_class_init(XwFixedClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->get_preferred_width = xw_fixed_get_preferred_size;
    widget_class->get_preferred_height = xw_fixed_get_preferred_size;
}

static void xw_fixed_init(XwFixed *)
{
}

// Recursively ensures h has its own real, positioned GtkFixed widget, creating one
// (and recursively ensuring its own parent has one first) if it doesn't already.
// A true toplevel (h->m_oswidget already set by SWELL itself) is the recursion's
// base case -- we never touch m_oswidget, only ever read it. Returns the widget to
// embed into, or nullptr if something in the chain isn't embeddable.
static void detach_bridge_widgets(GtkWidget *widget);

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
        GtkWidget *fixed = GTK_WIDGET(g_object_new(xw_fixed_get_type(), NULL));
        if (g_swell_wayland_title_h && (h->m_style & WS_CAPTION)) {
            gtk_widget_set_margin_start(fixed, SWELL_WAYLAND_BORDER_WIDTH);
            gtk_widget_set_margin_end(fixed, SWELL_WAYLAND_BORDER_WIDTH);
            gtk_widget_set_margin_top(fixed, g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH);
            gtk_widget_set_margin_bottom(fixed, SWELL_WAYLAND_BORDER_WIDTH);
        }
        gtk_container_add(GTK_CONTAINER(top), fixed);
        gtk_widget_show(fixed);
        return fixed;
    }

    // No size request on these intermediate fixeds: it was only refreshed when a
    // bridge xw_size ran, so after REAPER resized h it went stale -- and GTK won't
    // let a toplevel shrink below its content's request, so a host window could
    // never get smaller than its previous plugin. Sized by their children instead.
    GtkWidget *parent_widget = xw_ensure_embed_widget(h->m_parent);
    if (!parent_widget || !GTK_IS_FIXED(parent_widget)) return nullptr;
    const int scale = swell_gdk_widget_scale(parent_widget);
    const int x = swell_gdk_floor(h->m_position.left, scale);
    const int y = swell_gdk_floor(h->m_position.top, scale);

    GtkWidget *existing = (GtkWidget*)g_object_get_data(G_OBJECT(parent_widget), "xw-active-embed-fixed");
    if (existing && g_object_get_data(G_OBJECT(existing), "xw-embed-hwnd") == h &&
        gtk_widget_get_parent(existing) == parent_widget) {
        gtk_fixed_move(GTK_FIXED(parent_widget), existing, x, y);
        return existing;
    }

    if (existing) {
        detach_bridge_widgets(existing);
        gtk_widget_destroy(existing);
    }

    GtkWidget *fixed = gtk_fixed_new();
    gtk_fixed_put(GTK_FIXED(parent_widget), fixed, x, y);
    gtk_widget_show(fixed);

    g_object_set_data(G_OBJECT(fixed), "xw-embed-hwnd", h);
    g_object_set_data_full(G_OBJECT(parent_widget), "xw-active-embed-fixed", g_object_ref(fixed),
                          [](gpointer widget) { g_object_unref(widget); });
    return fixed;
}

// Size the container and centre it on Xwayland's screen. Rootless Xwayland's screen is
// exactly the monitor area and its pointer is clamped to it, so a container at 0,0
// leaves no room left/up of the plugin: knob drags stop dead there. Centring splits
// the spare room evenly; a plugin bigger than the screen clamps back to 0, so this
// never pushes a window further off-screen than 0,0 did.
static GdkRectangle place_container(xcb_connection_t *connection, xcb_window_t container, GtkWidget *widget, int w, int h)
{
    const GdkRectangle bounds = capture_monitor(connection, widget);
    const int x = bounds.x + wdl_max((bounds.width - w) / 2, 0);
    const int y = bounds.y + wdl_max((bounds.height - h) / 2, 0);
    swell_xcb_move_resize(connection, container, x, y, w, h);
    return bounds;
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
    if (!bs->cap && bs->connection && bs->parent) {
        xcb_window_t plugin = first_child(bs->connection, bs->parent);
        if (plugin) {
            xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(bs->connection,
                xcb_get_geometry(bs->connection, plugin), nullptr);
            if (!geometry) return false;
            const GdkRectangle bounds = place_container(bs->connection, bs->parent, hwnd->m_oswidget,
                                                        geometry->width, geometry->height);
            free(geometry);
            xcb_flush(bs->connection);
            Capture *c = setup_capture(bs->connection, bs->parent, plugin, hwnd);
            c->widget = hwnd->m_oswidget;
            c->monitor_bounds = bounds;

            // Wine plugins nest a child GUI window; native plugins draw directly.
            c->gui_win = first_child(bs->connection, plugin);
            if (!c->gui_win) c->gui_win = plugin;
            g_captures[c->gui_win] = c;
            connect_widget(c);
            bs->cap = c;
        }
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
    hwnd->m_private_data = 0;
    KillTimer(hwnd, 1);
    if (bs->cap) cleanup_capture(bs->cap);
    else if (bs->connection) {
        xcb_query_tree_reply_t *tree = xcb_query_tree_reply(bs->connection,
            xcb_query_tree(bs->connection, bs->parent), nullptr);
        if (tree) {
            const xcb_window_t *children = xcb_query_tree_children(tree);
            for (int i = 0; i < xcb_query_tree_children_length(tree); ++i)
                xcb_xfixes_change_save_set(bs->connection, XCB_SET_MODE_INSERT,
                    XCB_XFIXES_SAVE_SET_TARGET_ROOT, XCB_XFIXES_SAVE_SET_MAPPING_UNMAP, children[i]);
            free(tree);
        }
    }
    if (bs->connection) {
        xcb_render_util_disconnect(bs->connection);
        xcb_flush(bs->connection);
        xcb_disconnect(bs->connection);
    }
    if (hwnd->m_oswidget && GTK_IS_WIDGET(hwnd->m_oswidget)) {
        g_signal_handlers_disconnect_by_data(hwnd->m_oswidget, hwnd);
        gtk_widget_destroy(hwnd->m_oswidget);
        g_object_unref(hwnd->m_oswidget);
        hwnd->m_oswidget = nullptr;
    }
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
    // SWELL replaces the host toplevel when the FX window is floated/unfloated
    // from the FX list. Reattach the drawing area to the new GtkWindow.
    //
    // Doing this here rather than from the damage callback matters: xw_size
    // runs on WM_SIZE/WM_MOVE, after SWELL has finished rebuilding the window,
    // which is exactly where the X11 bridge does it. Attempting the same from
    // a damage callback ran mid-teardown and segfaulted.
    {
        // Re-resolve the embed container every time and re-place if it moved.
        // Fixed float/unfloat in fx list view
        GtkWidget *fresh = xw_ensure_embed_widget(hwnd->m_parent);
        if (fresh != bs->embed_container || gtk_widget_get_parent(hwnd->m_oswidget) != fresh) {
            DEBUG_PRINT("[REPARENT] embed container %p -> %p, re-placing\n",
                        (void*)bs->embed_container, (void*)fresh);
            bs->embed_container = fresh;
            bs->placed = false;   // the put/show path below re-places it
        }
    }

    GtkWidget *container = bs->embed_container;
    if (!container || !GTK_IS_FIXED(container)) return;
    const int scale = swell_gdk_widget_scale(container);

    RECT r = hwnd->m_position;
    int pos_x = r.left, pos_y = r.top;
    int w = r.right - r.left, h = r.bottom - r.top;

    HWND top = hwnd->m_parent;
    while (top && !top->m_oswidget) top = top->m_parent;
    if (top && top->m_menu) {
        pos_y += GetSystemMetrics(SM_CYMENU);
    }

    bool size_changed = !bs->has_last_size_pos ||
        r.left != bs->last_size_pos.left || r.top != bs->last_size_pos.top ||
        r.right != bs->last_size_pos.right || r.bottom != bs->last_size_pos.bottom;

    // Refresh the pixmap on the next draw after resizing. Guard on bs->cap:
    // WM_SIZE/SetWindowPos can fire before the plugin is captured (e.g. during
    // an FX-list swap), and the container resize also needs a live capture.
    if (bs->cap && size_changed) {
        refresh_pixmap(bs->cap);
        bs->cap->monitor_bounds = place_container(bs->cap->connection, bs->cap->parent_win,
                                                   hwnd->m_oswidget, w, h);
        xcb_flush(bs->cap->connection);
    }

    pos_x = swell_gdk_floor(pos_x, scale);
    pos_y = swell_gdk_floor(pos_y, scale);
    w = swell_gdk_ceil(w, scale);
    h = swell_gdk_ceil(h, scale);

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
                if (bs->cap) { connect_toplevel(bs->cap); refresh_gtk_offset(bs->cap); }
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
    if (bs->cap) { connect_toplevel(bs->cap); refresh_gtk_offset(bs->cap); }
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

static void xw_scale_changed(GtkWidget *, GParamSpec *, gpointer data)
{
    xw_size((HWND)data);
}

struct XwView { GtkDrawingArea parent; };
struct XwViewClass { GtkDrawingAreaClass parent; };
G_DEFINE_TYPE(XwView, xw_view, GTK_TYPE_DRAWING_AREA)

static void xw_view_realize(GtkWidget *widget)
{
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    GdkWindowAttr attr = {};
    attr.window_type = GDK_WINDOW_SUBSURFACE;
    attr.wclass = GDK_INPUT_OUTPUT;
    attr.x = allocation.x;
    attr.y = allocation.y;
    attr.width = allocation.width;
    attr.height = allocation.height;
    attr.visual = gtk_widget_get_visual(widget);
    attr.event_mask = gtk_widget_get_events(widget) | GDK_EXPOSURE_MASK;
    GdkWindow *window = gdk_window_new(gdk_screen_get_root_window(gtk_widget_get_screen(widget)), &attr,
                                      GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
    gdk_window_set_transient_for(window, gtk_widget_get_window(gtk_widget_get_toplevel(widget)));
    gtk_widget_set_realized(widget, TRUE);
    gtk_widget_register_window(widget, window);
    gtk_widget_set_window(widget, window);
}

static void xw_view_class_init(XwViewClass *klass)
{
    GTK_WIDGET_CLASS(klass)->realize = xw_view_realize;
    GTK_WIDGET_CLASS(klass)->unmap = [](GtkWidget *widget) {
        Capture *c = find_capture(gtk_widget_get_window(widget));
        if (c) c->gl.reset();
        GTK_WIDGET_CLASS(xw_view_parent_class)->unmap(widget);
    };
}

static void xw_view_init(XwView *)
{
}

static void detach_bridge_widgets(GtkWidget *widget)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE(widget, xw_view_get_type())) {
        GtkWidget *parent = gtk_widget_get_parent(widget);
        if (parent) {
            g_signal_handlers_block_matched(widget, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, (gpointer)xw_scale_changed, NULL);
            gtk_container_remove(GTK_CONTAINER(parent), widget);
            g_signal_handlers_unblock_matched(widget, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, (gpointer)xw_scale_changed, NULL);
        }
    } else if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *p = children; p; p = p->next) detach_bridge_widgets(GTK_WIDGET(p->data));
        g_list_free(children);
    }
}

void xw_bridge_detach_children(HWND hwnd)
{
    detach_bridge_widgets(hwnd->m_oswidget);
}

HWND xw_bridge_create(HWND viewpar, void **wref, const RECT *r, const char *bridge_class_name)
{
    HWND hwnd = nullptr;
    *wref = nullptr;

    if (!g_wm) init_private_xwayland();
    xcb_connection_t *connection = g_wm ? xcb_connect(XWaylandWM::display_name(), nullptr) : nullptr;
    if (!connection || xcb_connection_has_error(connection)) {
        if (connection) xcb_disconnect(connection);
        hwnd = new HWND__(viewpar, 0, r, NULL, false, NULL);
        hwnd->m_classname = bridge_class_name;
        return hwnd;
    }
    const auto composite = xcb_composite_query_version(connection, 0, 4);
    const auto fixes = xcb_xfixes_query_version(connection, 5, 0);
    const auto randr = xcb_randr_query_version(connection, 1, 5);
    free(xcb_composite_query_version_reply(connection, composite, nullptr));
    free(xcb_xfixes_query_version_reply(connection, fixes, nullptr));
    free(xcb_randr_query_version_reply(connection, randr, nullptr));
    xcb_screen_t *screen = swell_xcb_screen(connection);
    int w = wdl_max(r->right - r->left, 1);
    int h = wdl_max(r->bottom - r->top, 1);
    HWND parent = viewpar;
    while (parent && !parent->m_oswidget) parent = parent->m_parent;

    xcb_window_t container = xcb_generate_id(connection);
    const uint32_t background = screen->white_pixel;
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, container, screen->root, 0, 0, w, h, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_BACK_PIXEL, &background);
    place_container(connection, container, parent ? parent->m_oswidget : nullptr, w, h);
    xcb_map_window(connection, container);
    xcb_flush(connection);

    GtkWidget *draw_area = GTK_WIDGET(g_object_new(xw_view_get_type(), nullptr));
    g_object_ref_sink(draw_area);
    const int scale = swell_gdk_widget_scale(parent ? parent->m_oswidget : NULL);
    gtk_widget_set_size_request(draw_area, swell_gdk_ceil(w, scale), swell_gdk_ceil(h, scale));

    hwnd = new HWND__(viewpar, 0, r, NULL, true, xw_bridgeProc);
    hwnd->m_classname = bridge_class_name;
    hwnd->m_oswidget  = draw_area;

    bridgeState *bs = new bridgeState();
    bs->connection   = connection;
    bs->parent = container;
    hwnd->m_private_data = (INT_PTR)bs;
    g_signal_connect(draw_area, "notify::scale-factor", G_CALLBACK(xw_scale_changed), hwnd);

    *wref = (void*)(uintptr_t)container;

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
        if (!c || kv.first != c->parent_win) continue;
        if (!c || !c->connection || c->modals.empty()) continue;

        Capture::ModalWin &md = c->modals.back();
        xcb_window_t target = md.x11_win;
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
    if (!c || !c->connection) return false;

    xcb_window_t target = (c->root_popup && !c->popups.empty()) ? c->root_popup
                  : (c->gui_win ? c->gui_win : c->plugin_win);
    if (!target) return false;

    forward_key(c->connection, target, keycode, state, is_press);
    return true;
}
