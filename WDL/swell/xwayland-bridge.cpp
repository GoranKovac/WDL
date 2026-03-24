#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

#include "xwayland-bridge.h"

#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL 1

static pid_t s_xwayland_pid = 0;

static int x11_error_handler(Display *dpy, XErrorEvent *err)
{
    if (err->error_code == BadWindow ||
        err->error_code == BadDrawable ||
        err->error_code == BadMatch) return 0;

#ifdef _DEBUG
    char buf[256];
    XGetErrorText(dpy, err->error_code, buf, sizeof(buf));
    DEBUG_PRINT("X11 Error (non-fatal): %s (request %d, resource 0x%lx)\n",
                buf, err->request_code, err->resourceid);
#endif
    return 0;
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
    XSetErrorHandler(x11_error_handler);
    XInitThreads();
}

enum WindowType {
    WINDOW_PLUGIN = 0,
    WINDOW_MODAL  = 1,
    WINDOW_POPUP  = 2
};

struct X11CaptureState {
    Display *dpy;
    Window parent_win;
    Window plugin_win;
    Window main_plugin_gui;
    int gtk_x, gtk_y;
    Pixmap backing_pixmap;
    std::map<Window, GtkWidget*> child_windows;
    GtkWidget *plugin_widget;

    Pixmap dnd_pixmap;
    Window dnd_win;
    int dnd_x, dnd_y;
};
struct WindowRenderData {
    Display *dpy;
    Window x11_win;
    Window parent_win;
    Pixmap backing_pixmap;
    HWND hwnd;
    WindowType type;
    X11CaptureState *capture;
};


struct bridgeState {
    bridgeState(bool needrep, GdkWindow *_w, Window _nw, Display *_disp, GdkWindow *_curpar, HWND _hwnd_child);
    ~bridgeState();

    GdkWindow *w;
    Window native_w;
    Display *native_disp;
    GdkWindow *cur_parent;
    HWND hwnd_child;

    bool lastvis;
    bool need_reparent;
    RECT lastrect;

    GLXContext gl_ctx;
    void *x11_capture;
};

static void set_wm_state(Display *dpy, Window win, int state)
{
    XWindowAttributes attr;
    if (XGetWindowAttributes(dpy, win, &attr) == 0) return;

    Atom wm_state = XInternAtom(dpy, "WM_STATE", False);
    struct { long state; Window icon; } data;
    data.state = state;
    data.icon = None;

    XChangeProperty(dpy, win, wm_state, wm_state, 32,
                    PropModeReplace, (unsigned char*)&data, 2);
    XFlush(dpy);
}

static X11CaptureState* setup_x11_capture(Display *dpy, Window parent_win, Window plugin_win, int width, int height)
{
    X11CaptureState *state = new X11CaptureState();
    state->dpy = dpy;
    state->parent_win = parent_win;
    state->plugin_win = plugin_win;
    state->main_plugin_gui = 0;
    state->gtk_x = 0;
    state->gtk_y = 0;
    state->plugin_widget = NULL;
    state->backing_pixmap = 0;

    state->dnd_pixmap = 0;
    state->dnd_win = 0;
    state->dnd_x = 0;
    state->dnd_y = 0;

    set_wm_state(dpy, plugin_win, WM_STATE_NORMAL);

    XSelectInput(dpy, plugin_win,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask | SubstructureNotifyMask);

    XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureNotifyMask);
    XFlush(dpy);

    XCompositeRedirectWindow(dpy, plugin_win, CompositeRedirectAutomatic);
    state->backing_pixmap = XCompositeNameWindowPixmap(dpy, plugin_win);

    return state;
}

struct MotifWmHints {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long input_mode;
    unsigned long status;
};

#define MWM_HINTS_DECORATIONS (1L << 1)

static bool hasMotifHints(Display *dpy, Window win, MotifWmHints &hints_out)
{
    Atom atom_motif_hints = XInternAtom(dpy, "_MOTIF_WM_HINTS", True);
    if (atom_motif_hints == None) return false;

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = nullptr;

    int status = XGetWindowProperty(dpy, win, atom_motif_hints, 0, 5, False, atom_motif_hints,
                                    &actual_type, &actual_format, &nitems, &bytes_after, &prop);

    if (status != Success || !prop || actual_format != 32 || nitems < 5)
    {
        if (prop) XFree(prop);
        return false;
    }

    const long *l = reinterpret_cast<long*>(prop);
    hints_out.flags       = l[0];
    hints_out.functions   = l[1];
    hints_out.decorations = l[2];
    hints_out.input_mode  = l[3];
    hints_out.status      = l[4];
    XFree(prop);
    return true;
}

static bool is_window_from_owned_plugin(Display *dpy, Window win, X11CaptureState *state)
{
    Atom atom_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    pid_t window_pid = 0;
    if (XGetWindowProperty(dpy, win, atom_pid, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success)
    {
        if (prop && nitems > 0) { window_pid = *((pid_t*)prop); XFree(prop); }
    }

    pid_t our_pid = 0;
    if (XGetWindowProperty(dpy, state->main_plugin_gui, atom_pid, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success)
    {
        if (prop && nitems > 0) { our_pid = *((pid_t*)prop); XFree(prop); }
    }

    return (window_pid != 0 && our_pid != 0 && window_pid == our_pid);
}

static bool classify_popup(Display *dpy, Window win, XWindowAttributes *attr)
{
    if (!dpy || !win) return false;

    MotifWmHints hints;
    bool motif_popup = false;
    if (hasMotifHints(dpy, win, hints))
        motif_popup = (hints.flags & MWM_HINTS_DECORATIONS) && hints.decorations == 0;

    Atom atom_window_type        = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom atom_type_normal        = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    Atom atom_type_dialog        = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    Atom atom_type_popup_menu    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    Atom atom_type_menu          = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    Atom atom_type_dropdown_menu = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
    Atom atom_type_tooltip       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
    Atom atom_type_dnd           = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DND", False);
    Atom atom_type_utility       = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);

    bool override_redirect = attr->override_redirect;
    bool is_popup = override_redirect;

    Window transient_for = None;
    XGetTransientForHint(dpy, win, &transient_for);

    std::vector<Atom> window_types;
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *prop = nullptr;

    if (XGetWindowProperty(dpy, win, atom_window_type, 0, 10,
                           False, XA_ATOM, &actual_type, &actual_format,
                           &nitems, &bytes_after, &prop) == Success)
    {
        if (prop && actual_format == 32 && nitems > 0)
        {
            Atom *atoms = (Atom*)prop;
            window_types.assign(atoms, atoms + nitems);
        }
        if (prop) XFree(prop);
    }

    if (window_types.empty())
    {
        if (!override_redirect && transient_for != None)
            window_types.push_back(atom_type_dialog);
        else
            window_types.push_back(atom_type_normal);
    }

    for (Atom ty : window_types)
    {
        if (ty == atom_type_normal)
            is_popup = override_redirect || motif_popup;
        else if (ty == atom_type_dialog || ty == atom_type_utility)
            is_popup = override_redirect;
        else if (ty == atom_type_popup_menu || ty == atom_type_menu ||
                ty == atom_type_dropdown_menu || ty == atom_type_tooltip ||
                ty == atom_type_dnd)
            is_popup = true;
        else
            continue;
        break;
    }

    return is_popup;
}

static void window_render_data_destroy(gpointer data, GClosure *closure)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (rd->backing_pixmap && rd->type != WINDOW_PLUGIN)
        XFreePixmap(rd->dpy, rd->backing_pixmap);
    delete rd;
}

static gboolean plugin_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy) return FALSE;

    // For plugin, always use capture's backing_pixmap so resize updates work
    Pixmap pixmap = (rd->type == WINDOW_PLUGIN && rd->capture)
        ? rd->capture->backing_pixmap
        : rd->backing_pixmap;

    if (!pixmap) return FALSE;

    Window root_ret;
    int x, y;
    unsigned int w, h, border, depth;

    if (XGetGeometry(rd->dpy, pixmap, &root_ret, &x, &y, &w, &h, &border, &depth))
    {
        XWindowAttributes win_attr;
        Visual *visual = DefaultVisual(rd->dpy, DefaultScreen(rd->dpy));
        if (XGetWindowAttributes(rd->dpy, rd->x11_win, &win_attr))
            visual = win_attr.visual;

        cairo_surface_t *surface = cairo_xlib_surface_create(
            rd->dpy, pixmap, visual, w, h);
        if (surface)
        {
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_paint(cr);
            cairo_surface_destroy(surface);
        }
    }
    if (rd->capture && rd->capture->dnd_pixmap)
    {
        int px = rd->capture->dnd_x - rd->capture->gtk_x;
        int py = rd->capture->dnd_y - rd->capture->gtk_y;

        if (XGetGeometry(rd->dpy, rd->capture->dnd_pixmap, &root_ret, &x, &y, &w, &h, &border, &depth))
        {
            XWindowAttributes win_attr;
            Visual *visual = DefaultVisual(rd->dpy, DefaultScreen(rd->dpy));
            if (XGetWindowAttributes(rd->dpy, rd->capture->dnd_win, &win_attr))
                visual = win_attr.visual;

            cairo_surface_t *surface = cairo_xlib_surface_create(
                rd->dpy, rd->capture->dnd_pixmap,
                visual, w, h);
            if (surface)
            {
                cairo_set_source_surface(cr, surface, px, py);
                cairo_paint(cr);
                cairo_surface_destroy(surface);
            }
        }
    }

    return TRUE;
}

static gboolean window_button_press(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;

    if (rd->type == WINDOW_PLUGIN)
    {
        if (rd->hwnd) SetFocus(rd->hwnd);
        XRaiseWindow(rd->dpy, rd->parent_win);
        XFlush(rd->dpy);
        XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), (int)e->x, (int)e->y, CurrentTime);
    }
    else
{
        Window child_return;
        int win_x, win_y;
        XTranslateCoordinates(rd->dpy, rd->x11_win, DefaultRootWindow(rd->dpy),
                              (int)e->x, (int)e->y, &win_x, &win_y, &child_return);
        XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), win_x, win_y, CurrentTime);
    }

    XTestFakeButtonEvent(rd->dpy, e->button, True, CurrentTime);
    XFlush(rd->dpy);
    return TRUE;
}

static gboolean window_button_release(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;

    if (rd->type != WINDOW_PLUGIN)
    {
        Window child_return;
        int win_x, win_y;
        XTranslateCoordinates(rd->dpy, rd->x11_win, DefaultRootWindow(rd->dpy),
                              (int)e->x, (int)e->y, &win_x, &win_y, &child_return);
        XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), win_x, win_y, CurrentTime);
    }

    XTestFakeButtonEvent(rd->dpy, e->button, False, CurrentTime);
    XFlush(rd->dpy);
    return TRUE;
}

static gboolean window_motion(GtkWidget *widget, GdkEventMotion *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;

    if (rd->type == WINDOW_PLUGIN)
    {
        XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), (int)e->x, (int)e->y, CurrentTime);
    }
    else
{
        Window child_return;
        int win_x, win_y;
        XTranslateCoordinates(rd->dpy, rd->x11_win, DefaultRootWindow(rd->dpy),
                              (int)e->x, (int)e->y, &win_x, &win_y, &child_return);
        XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), win_x, win_y, CurrentTime);
    }

    XFlush(rd->dpy);
    return TRUE;
}

static gboolean window_scroll(GtkWidget *widget, GdkEventScroll *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;

    // Map GDK scroll direction to X11 button (4=up, 5=down, 6=left, 7=right)
    unsigned int button = 0;
    switch (e->direction)
    {
        case GDK_SCROLL_UP:    button = 4; break;
        case GDK_SCROLL_DOWN:  button = 5; break;
        case GDK_SCROLL_LEFT:  button = 6; break;
        case GDK_SCROLL_RIGHT: button = 7; break;
        default: return FALSE;
    }

    if (rd->type == WINDOW_PLUGIN)
    {
        XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), (int)e->x, (int)e->y, CurrentTime);
    }
    else
    {
        Window child_return;
        int win_x, win_y;
        XTranslateCoordinates(rd->dpy, rd->x11_win, DefaultRootWindow(rd->dpy),
                              (int)e->x, (int)e->y, &win_x, &win_y, &child_return);
        XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), win_x, win_y, CurrentTime);
    }

    XTestFakeButtonEvent(rd->dpy, button, True, CurrentTime);
    XFlush(rd->dpy);
    XTestFakeButtonEvent(rd->dpy, button, False, CurrentTime);
    XFlush(rd->dpy);

    return TRUE;
}

static void send_x11_key(Display *dpy, Window win, GdkEventKey *e, bool is_press)
{
    XKeyEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.type = is_press ? KeyPress : KeyRelease;
    xev.display = dpy;
    xev.window = win;
    xev.root = DefaultRootWindow(dpy);
    xev.time = CurrentTime;
    xev.keycode = e->hardware_keycode;
    xev.state = e->state;
    xev.same_screen = True;

    XSendEvent(dpy, win, True, is_press ? KeyPressMask : KeyReleaseMask, (XEvent*)&xev);
    XFlush(dpy);
}

static gboolean window_key_press(GtkWidget *widget, GdkEventKey *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;
    send_x11_key(rd->dpy, rd->x11_win, e, true);
    return TRUE;
}

static gboolean window_key_release(GtkWidget *widget, GdkEventKey *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;
    send_x11_key(rd->dpy, rd->x11_win, e, false);
    return TRUE;
}

bool xw_forward_key(HWND hwnd, int keycode, int state, bool is_press)
{
    if (!hwnd || !hwnd->m_private_data) return false;

    bridgeState *bs = (bridgeState*)hwnd->m_private_data;
    X11CaptureState *capture = (X11CaptureState*)bs->x11_capture;
    if (!capture || !capture->dpy || !capture->main_plugin_gui) return false;

    XKeyEvent xev;
    memset(&xev, 0, sizeof(xev));
    xev.type = is_press ? KeyPress : KeyRelease;
    xev.display = capture->dpy;
    xev.window = capture->main_plugin_gui;
    xev.root = DefaultRootWindow(capture->dpy);
    xev.time = CurrentTime;
    xev.keycode = keycode;
    xev.state = state;
    xev.same_screen = True;

    XSendEvent(capture->dpy, capture->main_plugin_gui, True,
               is_press ? KeyPressMask : KeyReleaseMask, (XEvent*)&xev);
    XFlush(capture->dpy);

    return true;
}

static void connect_window_signals(GtkWidget *event_widget, GtkWidget *key_widget, WindowRenderData *rd)
{
    gtk_widget_add_events(event_widget,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);

    g_signal_connect_data(event_widget, "draw",
                          G_CALLBACK(plugin_draw_callback), rd, window_render_data_destroy, (GConnectFlags)0);
    g_signal_connect_data(event_widget, "button-press-event",
                          G_CALLBACK(window_button_press), rd, NULL, (GConnectFlags)0);
    g_signal_connect_data(event_widget, "button-release-event",
                          G_CALLBACK(window_button_release), rd, NULL, (GConnectFlags)0);
    g_signal_connect_data(event_widget, "motion-notify-event",
                          G_CALLBACK(window_motion), rd, NULL, (GConnectFlags)0);
    g_signal_connect_data(event_widget, "scroll-event",
                          G_CALLBACK(window_scroll), rd, NULL, (GConnectFlags)0);

    if (key_widget)
    {
        gtk_widget_add_events(key_widget, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
        g_signal_connect_data(key_widget, "key-press-event",
                              G_CALLBACK(window_key_press), rd, NULL, (GConnectFlags)0);
        g_signal_connect_data(key_widget, "key-release-event",
                              G_CALLBACK(window_key_release), rd, NULL, (GConnectFlags)0);
    }
}

static void handle_new_plugin_window(Display *dpy, Window win, Window parent_win, X11CaptureState *state, XWindowAttributes *attr, HWND hwnd)
{
    if (!hwnd->m_oswidget) return;

    WindowRenderData *rd = new WindowRenderData();
    rd->dpy = dpy;
    rd->x11_win = win;
    rd->parent_win = parent_win;
    rd->backing_pixmap = state->backing_pixmap;
    rd->hwnd = hwnd;
    rd->type = WINDOW_PLUGIN;
    rd->capture = state;
    rd->backing_pixmap = 0;

    state->plugin_widget = hwnd->m_oswidget;

    connect_window_signals(hwnd->m_oswidget, NULL, rd);
    gtk_widget_queue_draw(hwnd->m_oswidget);
    DEBUG_PRINT("Plugin window set up: 0x%lx parent=0x%lx\n", win, parent_win);
}

static void handle_new_modal_window(Display *dpy, Window win, X11CaptureState *state, XWindowAttributes *attr, HWND hwnd)
{
    XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);
    Pixmap backing_pixmap = XCompositeNameWindowPixmap(dpy, win);

    GtkWidget *gtk_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(gtk_win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(gtk_win), FALSE);

    if (state->plugin_widget)
    {
        GtkWidget *toplevel = gtk_widget_get_toplevel(state->plugin_widget);
        if (toplevel && GTK_IS_WINDOW(toplevel))
            gtk_window_set_transient_for(GTK_WINDOW(gtk_win), GTK_WINDOW(toplevel));
    }

    gtk_window_resize(GTK_WINDOW(gtk_win), attr->width, attr->height);
    gtk_window_move(GTK_WINDOW(gtk_win), attr->x, attr->y);

    GtkWidget *draw_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area, attr->width, attr->height);
    gtk_container_add(GTK_CONTAINER(gtk_win), draw_area);

    WindowRenderData *rd = new WindowRenderData();
    rd->dpy = dpy;
    rd->x11_win = win;
    rd->parent_win = state->parent_win;
    rd->backing_pixmap = backing_pixmap;
    rd->hwnd = hwnd;
    rd->type = WINDOW_MODAL;
    rd->capture = NULL;

    connect_window_signals(draw_area, gtk_win, rd);
    gtk_widget_show_all(gtk_win);

    state->child_windows[win] = gtk_win;

    DEBUG_PRINT("Modal window created: 0x%lx size=%dx%d pos=%d,%d\n",
                win, attr->width, attr->height, attr->x, attr->y);
}

static void handle_new_popup_window(Display *dpy, Window win, X11CaptureState *state, XWindowAttributes *attr, HWND hwnd)
{
    //Juce fix.... shadows....
    if ((attr->width <= 1 || attr->height <= 1)) {
        return;
    }
    if ((attr->width == 12 || attr->height == 12)) {
        return;
    }

    // get top-most parent popup
    GtkWidget *parent_popup = nullptr;

    // look for last mapped popup
    for (auto it = state->child_windows.rbegin(); it != state->child_windows.rend(); ++it) {
        GtkWidget *popup = it->second;
        if (GTK_IS_WINDOW(popup) && gtk_widget_get_realized(popup)) {
            GdkWindow *gdk_win = gtk_widget_get_window(popup);
            if (gdk_win && gdk_window_is_visible(gdk_win)) {
                parent_popup = popup;
                break;
            }
        }
    }

    // fallback to main plugin window if none
    if (!parent_popup && state->plugin_widget) {
        parent_popup = gtk_widget_get_toplevel(state->plugin_widget);
    }

    XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);
    Pixmap backing_pixmap = XCompositeNameWindowPixmap(dpy, win);

    GtkWidget *gtk_win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_set_decorated(GTK_WINDOW(gtk_win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(gtk_win), FALSE);

    gtk_window_set_type_hint(GTK_WINDOW(gtk_win), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
    gtk_window_set_keep_above(GTK_WINDOW(gtk_win), TRUE);
    gtk_window_set_gravity(GTK_WINDOW(gtk_win), GDK_GRAVITY_STATIC);

    if (parent_popup) {
        gtk_window_set_transient_for(GTK_WINDOW(gtk_win), GTK_WINDOW(parent_popup));
        DEBUG_PRINT("  GTK transient set to parent popup %p\n", parent_popup);
    }

    // Translate X11 coords to absolute screen coords
    int screen_x = state->gtk_x + attr->x;
    int screen_y = state->gtk_y + attr->y;

    DEBUG_PRINT("Popup: x11 pos=%d,%d gtk_offset=%d,%d screen_pos=%d,%d size=%dx%d\n",
                attr->x, attr->y, state->gtk_x, state->gtk_y,
                screen_x, screen_y, attr->width, attr->height);

    gtk_window_move(GTK_WINDOW(gtk_win), screen_x, screen_y);
    gtk_window_resize(GTK_WINDOW(gtk_win), attr->width, attr->height);

    GtkWidget *draw_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area, attr->width, attr->height);
    gtk_container_add(GTK_CONTAINER(gtk_win), draw_area);

    WindowRenderData *rd = new WindowRenderData();
    rd->dpy = dpy;
    rd->x11_win = win;
    rd->parent_win = state->parent_win;
    rd->backing_pixmap = backing_pixmap;
    rd->hwnd = hwnd;
    rd->type = WINDOW_POPUP;
    rd->capture = NULL;

    connect_window_signals(draw_area, gtk_win, rd);
    gtk_widget_show_all(gtk_win);

    state->child_windows[win] = gtk_win;

    DEBUG_PRINT("Popup window created: 0x%lx size=%dx%d screen_pos=%d,%d\n",
                win, attr->width, attr->height, screen_x, screen_y);
}

static void handle_xs_events(bridgeState *bs, X11CaptureState *capture, HWND hwnd)
{
    while (XPending(bs->native_disp))
    {
        XEvent xev;
        XNextEvent(bs->native_disp, &xev);

        switch (xev.type)
        {
            case ConfigureNotify:
                {
                    Window configured_win = xev.xconfigure.window;
                    DEBUG_PRINT("ConfigureNotify: 0x%lx\n", configured_win);

                    if ((configured_win == capture->parent_win ||
                        configured_win == capture->plugin_win ||
                        configured_win == capture->main_plugin_gui) &&
                        capture->plugin_widget == NULL &&
                        capture->main_plugin_gui != 0)
                    {
                        DEBUG_PRINT("ConfigureNotify: triggering plugin setup for 0x%lx\n", capture->main_plugin_gui);
                        XWindowAttributes attr;
                        if (XGetWindowAttributes(capture->dpy, capture->main_plugin_gui, &attr))
                            handle_new_plugin_window(capture->dpy, capture->main_plugin_gui, capture->parent_win, capture, &attr, hwnd);
                    }

                    if ((configured_win == capture->plugin_win ||
                        configured_win == capture->main_plugin_gui) &&
                        capture->plugin_widget != NULL)
                    {
                        if (capture->backing_pixmap) XFreePixmap(capture->dpy, capture->backing_pixmap);
                        capture->backing_pixmap = XCompositeNameWindowPixmap(capture->dpy, capture->plugin_win);

                        // Resize X11 parent window to match plugin
                        XResizeWindow(capture->dpy, capture->parent_win, xev.xconfigure.width, xev.xconfigure.height);
                        XFlush(capture->dpy);

                        // Trigger xw_size to update GTK widget and container
                        SendMessage(hwnd, WM_SIZE, SIZE_RESTORED, 0);

                        if (capture->plugin_widget)
                            gtk_widget_queue_draw(capture->plugin_widget);
                    }

                    auto it = capture->child_windows.find(configured_win);
                    if (it != capture->child_windows.end())
                    {
                        if (capture->dnd_win != configured_win)
                        {
                            capture->dnd_win = configured_win;
                            gtk_widget_hide(it->second);
                        }
                    }

                    if (capture->dnd_win == configured_win)
                    {
                        if (capture->dnd_pixmap)
                        {
                            XFreePixmap(capture->dpy, capture->dnd_pixmap);
                            capture->dnd_pixmap = 0;
                        }

                        // Check window still exists before grabbing new pixmap
                        XWindowAttributes attr;
                        if (XGetWindowAttributes(capture->dpy, configured_win, &attr))
                            capture->dnd_pixmap = XCompositeNameWindowPixmap(capture->dpy, configured_win);

                        capture->dnd_x = capture->gtk_x + xev.xconfigure.x;
                        capture->dnd_y = capture->gtk_y + xev.xconfigure.y;
                        if (capture->plugin_widget)
                            gtk_widget_queue_draw(capture->plugin_widget);
                    } 
                }
                break;
            case MapNotify:
                {
                    Window mapped_win = xev.xmap.window;
                    DEBUG_PRINT("MapNotify: 0x%lx\n", mapped_win);

                    set_wm_state(capture->dpy, mapped_win, WM_STATE_NORMAL);

                    if (mapped_win == capture->parent_win ||
                        mapped_win == capture->plugin_win ||
                        mapped_win == capture->main_plugin_gui){
                        DEBUG_PRINT("MapNotify: skipping main plugin \n");
                        break;
                    }

                    if (capture->child_windows.find(mapped_win) != capture->child_windows.end())
                        break;

                    if (!is_window_from_owned_plugin(capture->dpy, mapped_win, capture))
                    {
                        DEBUG_PRINT("  Not from owned plugin, skipping\n");
                        break;
                    }

                    XWindowAttributes attr;
                    if (!XGetWindowAttributes(capture->dpy, mapped_win, &attr) ||
                        attr.map_state != IsViewable)
                        break;

                    bool is_popup = classify_popup(capture->dpy, mapped_win, &attr);
                    DEBUG_PRINT("  mapped_win=0x%lx is_popup=%d size=%dx%d\n", 
                                mapped_win, is_popup, attr.width, attr.height);

                    if (!is_popup)
                        handle_new_modal_window(capture->dpy, mapped_win, capture, &attr, hwnd);
                    else
                        handle_new_popup_window(capture->dpy, mapped_win, capture, &attr, hwnd);
                }
                break;
            case UnmapNotify:
                {
                    Window unmapped_win = xev.xunmap.window;
                    DEBUG_PRINT("UnmapNotify: 0x%lx\n", unmapped_win);

                    set_wm_state(capture->dpy, unmapped_win, WM_STATE_WITHDRAWN);

                    auto it = capture->child_windows.find(unmapped_win);
                    if (it != capture->child_windows.end())
                    {
                        gtk_widget_destroy(it->second);
                        capture->child_windows.erase(it);
                    }
                    if (unmapped_win == capture->dnd_win)
                    {
                        if (capture->dnd_pixmap)
                        {
                            XFreePixmap(capture->dpy, capture->dnd_pixmap);
                            capture->dnd_pixmap = 0;
                        }
                        capture->dnd_win = 0;
                    }
                }
                break;
        }
    }
}

static gboolean xw_capture_update(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return FALSE;

    bridgeState *bs = (bridgeState*)hwnd->m_private_data;
    if (!bs) return FALSE;

    X11CaptureState *capture = NULL;

    if (!bs->x11_capture && bs->native_disp && bs->native_w)
    {
        Window root, par, *list = NULL;
        unsigned int nlist = 0;
        if (XQueryTree(bs->native_disp, bs->native_w, &root, &par, &list, &nlist))
        {
            if (!list || !nlist)
            {
                if (list) XFree(list);
                return FALSE;
            }

            Window plugin_win = list[0];

            XWindowAttributes attr;
            int width, height;
            if (XGetWindowAttributes(bs->native_disp, plugin_win, &attr))
            {
                width = attr.width;
                height = attr.height;
                XResizeWindow(bs->native_disp, bs->native_w, width, height);
                XFlush(bs->native_disp);
            }
            else
        {
                RECT r = hwnd->m_position;
                width = wdl_max(r.right - r.left, 1);
                height = wdl_max(r.bottom - r.top, 1);
            }

            XFree(list);

            capture = setup_x11_capture(bs->native_disp, bs->native_w, plugin_win, width, height);
            bs->x11_capture = (void*)capture;

            Window gui_root, gui_par, *gui_list = NULL;
            unsigned int gui_nlist = 0;
            if (XQueryTree(bs->native_disp, plugin_win, &gui_root, &gui_par, &gui_list, &gui_nlist) && gui_nlist > 0)
            {
                capture->main_plugin_gui = gui_list[0];
                DEBUG_PRINT("Main plugin GUI: 0x%lx\n", capture->main_plugin_gui);
                XFree(gui_list);
            }
        }
    }

    if (!capture && bs->x11_capture)
        capture = (X11CaptureState*)bs->x11_capture;

    if (capture && bs->native_disp)
    {
        handle_xs_events(bs, capture, hwnd);

        std::vector<Window> to_remove;
        for (auto &pair : capture->child_windows)
        {
            XWindowAttributes attr;
            if (!XGetWindowAttributes(capture->dpy, pair.first, &attr))
            {
                gtk_widget_destroy(pair.second);
                to_remove.push_back(pair.first);
            }
        }
        for (Window win : to_remove)
        capture->child_windows.erase(win);
    }

    // Redraw plugin
    if (capture && hwnd->m_oswidget)
        gtk_widget_queue_draw(hwnd->m_oswidget);

    // Redraw modals/popups
    if (capture)
    {
        for (auto &pair : capture->child_windows)
        {
            GtkWidget *draw_area = gtk_bin_get_child(GTK_BIN(pair.second));
            if (draw_area && gtk_widget_get_visible(pair.second))
                gtk_widget_queue_draw(draw_area);
        }
    }

    return TRUE;
}

static void cleanup_x11_capture(X11CaptureState *state)
{
    if (!state) return;
    DEBUG_PRINT("Cleaning up X11 capture state\n");

    for (auto &pair : state->child_windows)
    {
        set_wm_state(state->dpy, pair.first, WM_STATE_WITHDRAWN);
        if (GTK_IS_WINDOW(pair.second))
            gtk_widget_destroy(pair.second);
    }
    state->child_windows.clear();

    if (state->dnd_pixmap) XFreePixmap(state->dpy, state->dnd_pixmap);
    if (state->backing_pixmap) XFreePixmap(state->dpy, state->backing_pixmap);

    Display *dpy_to_close = state->dpy;
    state->dpy = NULL;
    state->plugin_win = 0;
    state->plugin_widget = NULL;

    if (dpy_to_close)
        XCloseDisplay(dpy_to_close);

    delete state;
}

void xw_destroy(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return;

    bridgeState *bs = (bridgeState*)hwnd->m_private_data;

    if (hwnd->m_oswidget)
    {
        GtkWidget *parent = gtk_widget_get_parent(hwnd->m_oswidget);
        if (parent)
            gtk_container_remove(GTK_CONTAINER(parent), hwnd->m_oswidget);
    }

    if (bs->x11_capture)
    {
        cleanup_x11_capture((X11CaptureState*)bs->x11_capture);
        bs->x11_capture = NULL;
    }

    hwnd->m_private_data = 0;
    delete bs;
}

void xw_size(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return;

    bridgeState *bs = (bridgeState*)hwnd->m_private_data;
    if (!hwnd->m_oswidget) return;

    HWND parent = hwnd->m_parent;
    while (parent && !parent->m_oswidget)
        parent = parent->m_parent;

    if (!parent || !parent->m_oswidget) return;

    GtkWidget *container = parent->m_oswidget;

    if (GTK_IS_WINDOW(container))
    {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(container));
        if (child)
            container = child;
        else
        {
            GtkWidget *fixed = gtk_fixed_new();
            gtk_container_add(GTK_CONTAINER(container), fixed);
            gtk_widget_show(fixed);
            container = fixed;
        }
    }

    RECT r = hwnd->m_position;
    int pos_x = 0;
    int pos_y = 0;

    HWND first_parent = hwnd->m_parent;
    if (first_parent)
    {
        pos_x = first_parent->m_position.left + r.left;
        pos_y = first_parent->m_position.top + r.top;

        // Add toolbar offset in embedded mode
        if (first_parent->m_parent)
        {
            RECT p0 = first_parent->m_position;
            RECT p1 = first_parent->m_parent->m_position;
            pos_y += p1.bottom - p0.bottom;
        }
    }

    // Store absolute screen position in capture state
    if (bs->x11_capture)
    {
        X11CaptureState *capture = (X11CaptureState*)bs->x11_capture;
        capture->gtk_x = pos_x;
        capture->gtk_y = pos_y;
    }

    if (GTK_IS_FIXED(container))
    {
        if (bs->need_reparent)
        {
            gtk_fixed_put(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
            gtk_widget_set_size_request(hwnd->m_oswidget, r.right - r.left, r.bottom - r.top);
            gtk_widget_show(hwnd->m_oswidget);
            bs->need_reparent = false;
        }
        else
    {
            gtk_fixed_move(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
            gtk_widget_set_size_request(hwnd->m_oswidget, r.right - r.left, r.bottom - r.top);
        }

        if (GTK_IS_WINDOW(parent->m_oswidget))
        {
            gtk_widget_set_size_request(GTK_WIDGET(parent->m_oswidget),
                                        pos_x + (r.right - r.left),
                                        pos_y + (r.bottom - r.top));
            gtk_widget_queue_resize(GTK_WIDGET(parent->m_oswidget));
        }
    }
    else if (GTK_IS_CONTAINER(container))
    {
        if (bs->need_reparent)
        {
            gtk_container_add(GTK_CONTAINER(container), hwnd->m_oswidget);
            gtk_widget_show(hwnd->m_oswidget);
            bs->need_reparent = false;
        }
    }
}

static LRESULT xw_bridgeProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_DESTROY: xw_destroy(hwnd); break;
        case WM_TIMER:   xw_capture_update(hwnd); break;
        case WM_MOVE:
        case WM_SIZE:    xw_size(hwnd); break;
    }
    return 0;
}

HWND xw_bridge_create(HWND viewpar, void **wref, const RECT *r, const char *bridge_class_name)
{
    HWND hwnd = NULL;
    *wref = NULL;

    Display *disp = XOpenDisplay(":10");
    if (!disp)
    {
        hwnd = new HWND__(viewpar, 0, r, NULL, false, NULL);
        hwnd->m_classname = bridge_class_name;
        return hwnd;
    }

    int screen = DefaultScreen(disp);
    Window root = RootWindow(disp, screen);

    Window w = XCreateSimpleWindow(disp, root, 0, 0,
                                   wdl_max(r->right - r->left, 1),
                                   wdl_max(r->bottom - r->top, 1),
                                   0, BlackPixel(disp, screen), WhitePixel(disp, screen));

    XMapWindow(disp, w);
    XFlush(disp);

    GtkWidget *draw_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(draw_area,
                                wdl_max(r->right - r->left, 1),
                                wdl_max(r->bottom - r->top, 1));

    hwnd = new HWND__(viewpar, 0, r, NULL, true, xw_bridgeProc);
    hwnd->m_classname = bridge_class_name;
    hwnd->m_oswidget = draw_area;

    bool need_reparent = true;
    bridgeState *bs = new bridgeState(need_reparent, NULL, w, disp, NULL, hwnd);
    hwnd->m_private_data = (INT_PTR)bs;

    if (w)
    {
        *wref = (void*)w;
        XSelectInput(disp, w, StructureNotifyMask | SubstructureNotifyMask);
        SetTimer(hwnd, 1, 16, NULL);
        if (!need_reparent)
            SendMessage(hwnd, WM_SIZE, SIZE_RESTORED, 0);
    }

    return hwnd;
}
