#ifdef _DEBUG
  #define X11_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
  #define X11_PRINT(...) ((void)0)
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
    X11_PRINT("X11 Error (non-fatal): %s (request %d, resource 0x%lx)\n",
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

//------------------------------------------------------------
// Structs
//------------------------------------------------------------

struct WindowRenderData {
    Display *dpy;
    Window x11_win;
    Window parent_win;
    Pixmap backing_pixmap;
    HWND hwnd;
};

struct X11CaptureState {
    Display *dpy;
    Window parent_win;
    Window plugin_win;
    Window main_plugin_gui;
    std::map<Window, GtkWidget*> child_windows;
    GtkWidget *plugin_widget;
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

//------------------------------------------------------------
// Helpers
//------------------------------------------------------------

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

//------------------------------------------------------------
// Draw callbacks
//------------------------------------------------------------

static void window_render_data_destroy(gpointer data, GClosure *closure)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (rd->backing_pixmap) XFreePixmap(rd->dpy, rd->backing_pixmap);
    delete rd;
}

static gboolean plugin_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->backing_pixmap) return FALSE;

    Window root_ret;
    int x, y;
    unsigned int w, h, border, depth;

    if (XGetGeometry(rd->dpy, rd->backing_pixmap, &root_ret, &x, &y, &w, &h, &border, &depth))
    {
        cairo_surface_t *surface = cairo_xlib_surface_create(
            rd->dpy, rd->backing_pixmap,
            DefaultVisual(rd->dpy, DefaultScreen(rd->dpy)),
            w, h);

        if (surface)
        {
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_paint(cr);
            cairo_surface_destroy(surface);
        }
    }

    return TRUE;
}

//------------------------------------------------------------
// Input handlers
//------------------------------------------------------------

static gboolean plugin_button_press(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;

    if (rd->hwnd) SetFocus(rd->hwnd);

    XRaiseWindow(rd->dpy, rd->parent_win);
    XFlush(rd->dpy);

    XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), (int)e->x, (int)e->y, CurrentTime);
    XTestFakeButtonEvent(rd->dpy, e->button, True, CurrentTime);
    XFlush(rd->dpy);
    return TRUE;
}

static gboolean plugin_button_release(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;

    XTestFakeButtonEvent(rd->dpy, e->button, False, CurrentTime);
    XFlush(rd->dpy);
    return TRUE;
}

static gboolean plugin_motion(GtkWidget *widget, GdkEventMotion *e, gpointer data)
{
    WindowRenderData *rd = (WindowRenderData*)data;
    if (!rd || !rd->dpy || !rd->x11_win) return FALSE;

    XTestFakeMotionEvent(rd->dpy, DefaultScreen(rd->dpy), (int)e->x, (int)e->y, CurrentTime);
    XFlush(rd->dpy);
    return TRUE;
}

//------------------------------------------------------------
// Key forwarding
//------------------------------------------------------------

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

//------------------------------------------------------------
// Window management
//------------------------------------------------------------

static void handle_new_plugin_window(Display *dpy, Window win, Window parent_win, X11CaptureState *state, XWindowAttributes *attr, HWND hwnd)
{
    if (!hwnd->m_oswidget) return;

    XCompositeRedirectWindow(dpy, win, CompositeRedirectAutomatic);
    Pixmap backing_pixmap = XCompositeNameWindowPixmap(dpy, win);

    WindowRenderData *rd = new WindowRenderData();
    rd->dpy = dpy;
    rd->x11_win = win;
    rd->parent_win = parent_win;
    rd->backing_pixmap = backing_pixmap;
    rd->hwnd = hwnd;

    state->plugin_widget = hwnd->m_oswidget;

    gtk_widget_add_events(hwnd->m_oswidget,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);

    g_signal_connect_data(hwnd->m_oswidget, "draw",
        G_CALLBACK(plugin_draw_callback), rd, window_render_data_destroy, (GConnectFlags)0);
    g_signal_connect_data(hwnd->m_oswidget, "button-press-event",
        G_CALLBACK(plugin_button_press), rd, NULL, (GConnectFlags)0);
    g_signal_connect_data(hwnd->m_oswidget, "button-release-event",
        G_CALLBACK(plugin_button_release), rd, NULL, (GConnectFlags)0);
    g_signal_connect_data(hwnd->m_oswidget, "motion-notify-event",
        G_CALLBACK(plugin_motion), rd, NULL, (GConnectFlags)0);

    gtk_widget_queue_draw(hwnd->m_oswidget);

    X11_PRINT("Plugin window set up: 0x%lx parent=0x%lx\n", win, parent_win);
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

                if ((configured_win == capture->parent_win ||
                     configured_win == capture->plugin_win ||
                     configured_win == capture->main_plugin_gui) &&
                    capture->plugin_widget == NULL &&
                    capture->main_plugin_gui != 0)
                {
                    X11_PRINT("ConfigureNotify: triggering plugin setup for 0x%lx\n", capture->main_plugin_gui);
                    XWindowAttributes attr;
                    if (XGetWindowAttributes(capture->dpy, capture->main_plugin_gui, &attr))
                        handle_new_plugin_window(capture->dpy, capture->main_plugin_gui, capture->parent_win, capture, &attr, hwnd);
                }
            }
            break;

            case MapNotify:
            {
                Window mapped_win = xev.xmap.window;
                X11_PRINT("MapNotify: 0x%lx\n", mapped_win);

                if (mapped_win == capture->parent_win ||
                    mapped_win == capture->plugin_win ||
                    mapped_win == capture->main_plugin_gui)
                    break;

                if (capture->child_windows.find(mapped_win) != capture->child_windows.end())
                    break;

                // Phase 2: handle modals
                // Phase 3: handle popups
            }
            break;

            case UnmapNotify:
            {
                Window unmapped_win = xev.xunmap.window;
                X11_PRINT("UnmapNotify: 0x%lx\n", unmapped_win);

                auto it = capture->child_windows.find(unmapped_win);
                if (it != capture->child_windows.end())
                {
                    set_wm_state(capture->dpy, unmapped_win, WM_STATE_WITHDRAWN);
                    gtk_widget_destroy(it->second);
                    capture->child_windows.erase(it);
                }
            }
            break;

            default: break;
        }
    }
}

static X11CaptureState* setup_x11_capture(Display *dpy, Window parent_win, Window plugin_win, int width, int height)
{
    X11CaptureState *state = new X11CaptureState();
    state->dpy = dpy;
    state->parent_win = parent_win;
    state->plugin_win = plugin_win;
    state->main_plugin_gui = 0;
    state->plugin_widget = NULL;

    set_wm_state(dpy, plugin_win, WM_STATE_NORMAL);

    XSelectInput(dpy, plugin_win,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask | SubstructureNotifyMask);

    XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureNotifyMask);
    XFlush(dpy);

    XCompositeRedirectWindow(dpy, plugin_win, CompositeRedirectAutomatic);

    return state;
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
                X11_PRINT("Main plugin GUI: 0x%lx\n", capture->main_plugin_gui);
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

    if (capture && hwnd->m_oswidget)
        gtk_widget_queue_draw(hwnd->m_oswidget);

    return TRUE;
}

//------------------------------------------------------------
// Cleanup
//------------------------------------------------------------

static void cleanup_x11_capture(X11CaptureState *state)
{
    if (!state) return;

    X11_PRINT("Cleaning up X11 capture state\n");

    for (auto &pair : state->child_windows)
    {
        set_wm_state(state->dpy, pair.first, WM_STATE_WITHDRAWN);
        if (GTK_IS_WINDOW(pair.second))
            gtk_widget_destroy(pair.second);
    }
    state->child_windows.clear();

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

        if (first_parent->m_parent)
        {
            RECT p0 = first_parent->m_position;
            RECT p1 = first_parent->m_parent->m_position;
            pos_y += p1.bottom - p0.bottom;
        }
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

//------------------------------------------------------------
// Bridge proc and create
//------------------------------------------------------------

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
