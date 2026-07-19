// xwayland-bridge-wm.cpp
// Minimal ICCCM/EWMH window manager for the XWayland bridge.
//
// Design goals
// ────────────
// 1. End the freeze: proper grab lifecycle (owner_events=True) so that clicking
//    outside a popup/modal never deadlocks input.
// 2. Native X11 plugin support: honour MapRequest, ConfigureRequest,
//    send WM_TAKE_FOCUS, set WM_STATE — exactly what plugins expect from a WM.
// 3. Stay minimal: we are NOT a full WM.  We only manage windows that are
//    children of our plugin containers.

#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

#include "xwayland-bridge-wm.h"
#include "xwayland-bridge.h"          // g_wm, g_wm_dpy
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xcomposite.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/prctl.h>
#include <signal.h>
#include <glib.h>
#include <algorithm>

// ─── Construction ─────────────────────────────────────────────────────────────
XWaylandWM::XWaylandWM(Display *dpy) : dpy_(dpy) {
    init_atoms();
}

// ─── Infrastructure: Xwayland spawn, error handling, event pump ───────────────
// This has nothing to do with the bridge's compositing/input work — it's the WM
// owning its own X connection, process, and event loop.

static pid_t s_xwayland_pid = 0;

static int wm_x11_error_handler(Display *dpy, XErrorEvent *err)
{
    // BadDamage: modal teardown can XDamageDestroy a damage the server already
    // auto-freed when the dialog window was destroyed. Ignore like other teardown
    // races. g_bridge_damage_error_base is set once the bridge queries XDamage.
    extern int g_bridge_damage_error_base;
    if (g_bridge_damage_error_base >= 0 &&
        err->error_code == g_bridge_damage_error_base) return 0;

    if (err->error_code == BadWindow   ||
        err->error_code == BadDrawable ||
        err->error_code == BadPixmap   ||
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

// GLib main-loop watch: pump all pending events. The WM handles each first, then
// the bridge gets a look via on_unhandled_event for its own per-event work.
static gboolean wm_gio_event_cb(GIOChannel *, GIOCondition, gpointer)
{
    while (g_wm_dpy && XPending(g_wm_dpy)) {
        XEvent ev;
        XNextEvent(g_wm_dpy, &ev);
        if (g_wm) {
            g_wm->handle_event(&ev);
            if (g_wm->on_unhandled_event) g_wm->on_unhandled_event(&ev);
        }
    }
    return TRUE;
}

XWaylandWM* XWaylandWM::init_bridge_wm(const char *display_name)
{
    if (s_xwayland_pid > 0) return g_wm;

    s_xwayland_pid = fork();
    if (s_xwayland_pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        // Xvfb rather than Xwayland: a real Xorg server rendering into a memory
        // framebuffer. Nothing is ever on-screen or occluded, so every plugin window
        // is fully rendered and capturable regardless of where it sits -- and the
        // pointer on this display is ours alone. Composite redirect also behaves
        // normally here; the input dead zone was a nested-Xwayland defect, not a
        // property of redirecting. The screen is deliberately huge so plugins can be
        // laid out without overlapping (occlusion still costs pixels on a plain X
        // server) and never clip at the edges.
        execl("/usr/bin/Xvfb", "Xvfb", display_name,
              "-screen", "0", "8192x8192x24",
              "+extension", "MIT-SHM",
              "+extension", "Composite",
              "+bs",
              "-nolisten", "tcp",
              NULL);
        _exit(1);
    }
    if (s_xwayland_pid < 0) return nullptr;

    setenv("DISPLAY", display_name, 1);
    XInitThreads();
    XSetErrorHandler(wm_x11_error_handler);

    for (int i = 0; i < 30; i++) {
        g_wm_dpy = XOpenDisplay(display_name);
        if (g_wm_dpy) break;
        usleep(100000);
    }
    if (!g_wm_dpy) { DEBUG_PRINT("[WM] Xwayland not ready\n"); return nullptr; }

    g_wm = new XWaylandWM(g_wm_dpy);

    Window support = XCreateSimpleWindow(g_wm_dpy, DefaultRootWindow(g_wm_dpy),
                                         -2, -2, 1, 1, 0, 0, 0);
    XMapWindow(g_wm_dpy, support);
    g_wm->announce_wm(support);

    GIOChannel *ch = g_io_channel_unix_new(ConnectionNumber(g_wm_dpy));
    g_io_add_watch(ch, G_IO_IN, wm_gio_event_cb, nullptr);
    g_io_channel_unref(ch);

    DEBUG_PRINT("[WM] initialised\n");
    return g_wm;
}

// ─── Atom initialisation ──────────────────────────────────────────────────────

void XWaylandWM::init_atoms() {
    // ICCCM
    atoms_.WM_PROTOCOLS       = XInternAtom(dpy_, "WM_PROTOCOLS",       False);
    atoms_.WM_DELETE_WINDOW   = XInternAtom(dpy_, "WM_DELETE_WINDOW",   False);
    atoms_.WM_TAKE_FOCUS      = XInternAtom(dpy_, "WM_TAKE_FOCUS",      False);
    atoms_.WM_STATE           = XInternAtom(dpy_, "WM_STATE",           False);
    atoms_.WM_CHANGE_STATE    = XInternAtom(dpy_, "WM_CHANGE_STATE",    False);
    atoms_.WM_HINTS           = XInternAtom(dpy_, "WM_HINTS",           False);
    atoms_.WM_NORMAL_HINTS    = XInternAtom(dpy_, "WM_NORMAL_HINTS",    False);
    atoms_.WM_TRANSIENT_FOR   = XInternAtom(dpy_, "WM_TRANSIENT_FOR",   False);
    atoms_.WM_NAME            = XInternAtom(dpy_, "WM_NAME",            False);
    atoms_.WM_CLASS           = XInternAtom(dpy_, "WM_CLASS",           False);
    // EWMH
    atoms_._NET_WM_STATE              = XInternAtom(dpy_, "_NET_WM_STATE",              False);
    atoms_._NET_WM_STATE_MODAL        = XInternAtom(dpy_, "_NET_WM_STATE_MODAL",        False);
    atoms_._NET_WM_STATE_ABOVE        = XInternAtom(dpy_, "_NET_WM_STATE_ABOVE",        False);
    atoms_._NET_ACTIVE_WINDOW         = XInternAtom(dpy_, "_NET_ACTIVE_WINDOW",         False);
    atoms_._NET_WM_WINDOW_TYPE        = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE",        False);
    atoms_._NET_WM_WINDOW_TYPE_DIALOG = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU
                                      = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    atoms_._NET_WM_WINDOW_TYPE_TOOLTIP= XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE_TOOLTIP",False);
    atoms_._NET_WM_WINDOW_TYPE_NORMAL = XInternAtom(dpy_, "_NET_WM_WINDOW_TYPE_NORMAL", False);
    atoms_._NET_SUPPORTING_WM_CHECK   = XInternAtom(dpy_, "_NET_SUPPORTING_WM_CHECK",   False);
    atoms_._NET_WM_NAME               = XInternAtom(dpy_, "_NET_WM_NAME",               False);
    atoms_._NET_SUPPORTED             = XInternAtom(dpy_, "_NET_SUPPORTED",             False);
    atoms_._NET_WM_BYPASS_COMPOSITOR  = XInternAtom(dpy_, "_NET_WM_BYPASS_COMPOSITOR",  False);
}

// ─── WM announcement (needed by native plugins that check for a WM) ───────────

void XWaylandWM::announce_wm(Window support_win) {
    support_win_ = support_win;
    Window root = DefaultRootWindow(dpy_);

    // _NET_SUPPORTING_WM_CHECK on root → support_win
    XChangeProperty(dpy_, root,
                    atoms_._NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char*)&support_win, 1);
    // Same on support_win itself
    XChangeProperty(dpy_, support_win,
                    atoms_._NET_SUPPORTING_WM_CHECK, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char*)&support_win, 1);

    // _NET_WM_NAME on support_win
    const char *wm_name = "REAPERXBridge";
    XChangeProperty(dpy_, support_win,
                    atoms_._NET_WM_NAME,
                    XInternAtom(dpy_, "UTF8_STRING", False), 8,
                    PropModeReplace, (const unsigned char*)wm_name, strlen(wm_name));

    // Advertise supported EWMH atoms
    Atom supported[] = {
        atoms_._NET_WM_STATE,
        atoms_._NET_WM_STATE_MODAL,
        atoms_._NET_WM_STATE_ABOVE,
        atoms_._NET_ACTIVE_WINDOW,
        atoms_._NET_WM_WINDOW_TYPE,
        atoms_._NET_WM_WINDOW_TYPE_DIALOG,
        atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU,
        atoms_._NET_WM_WINDOW_TYPE_NORMAL,
        atoms_._NET_SUPPORTING_WM_CHECK,
        atoms_._NET_WM_NAME,
    };
    XChangeProperty(dpy_, root,
                    atoms_._NET_SUPPORTED, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)supported,
                    sizeof(supported)/sizeof(supported[0]));

    XFlush(dpy_);
    DEBUG_PRINT("[WM] announced on root=0x%lx support_win=0x%lx\n", root, support_win);

    XSelectInput(dpy_, root,
                 SubstructureRedirectMask |
                 SubstructureNotifyMask   |
                 PropertyChangeMask);
    XFlush(dpy_);
    DEBUG_PRINT("[WM] SubstructureRedirect on root selected\n");
    
    // Verify it actually got set
    Atom actual; int fmt; unsigned long n, left;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy_, root, atoms_._NET_SUPPORTING_WM_CHECK,
                           0, 1, False, XA_WINDOW,
                           &actual, &fmt, &n, &left, &data) == Success && data) {
        Window set_win = *(Window*)data;
        DEBUG_PRINT("[WM] _NET_SUPPORTING_WM_CHECK verified: 0x%lx\n", set_win);
        XFree(data);
    } else {
        DEBUG_PRINT("[WM] _NET_SUPPORTING_WM_CHECK NOT SET - announce failed\n");
    }
}

// ─── Container registration ───────────────────────────────────────────────────
void XWaylandWM::register_container(Window container, bool is_native) {
    containers_.insert(container);
    xwm_redirect_container(dpy_, container);
    (void)is_native;
}

void XWaylandWM::unregister_container(Window container) {
    containers_.erase(container);
}

// ─── Window tracking ──────────────────────────────────────────────────────────
void XWaylandWM::track_window(Window w, Window parent, bool is_native_plugin) {
    if (states_.count(w)) return;

    WMWindowState st{};
    st.xid              = w;
    st.transient_for    = None;
    st.mapped           = false;
    st.is_native_plugin = is_native_plugin;
    st.input_hint       = true; // default: wants input

    // Read client-set hints
    read_wm_protocols(w, st);
    read_wm_hints(w, st);

    // Read WM_TRANSIENT_FOR
    Window trans = None;
    if (XGetTransientForHint(dpy_, w, &trans) && trans != None)
        st.transient_for = trans;

    // Read _NET_WM_WINDOW_TYPE for popup/modal detection
    Atom actual; int fmt; unsigned long n, left;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(dpy_, w, atoms_._NET_WM_WINDOW_TYPE,
                           0, 32, False, XA_ATOM,
                           &actual, &fmt, &n, &left, &data) == Success && data) {
        Atom *types = (Atom*)data;
        for (unsigned long i = 0; i < n; ++i) {
            if (types[i] == atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU ||
                types[i] == atoms_._NET_WM_WINDOW_TYPE_TOOLTIP)
                st.is_popup = true;
            if (types[i] == atoms_._NET_WM_WINDOW_TYPE_DIALOG)
                st.is_modal = true;
        }
        XFree(data);
    }

    // Subscribe to property changes so we can react to hint updates
    XSelectInput(dpy_, w, PropertyChangeMask | StructureNotifyMask);

    states_[w] = st;
}

void XWaylandWM::untrack_window(Window w) {
    states_.erase(w);
}

const WMWindowState* XWaylandWM::window_state(Window w) const {
    auto it = states_.find(w);
    return it == states_.end() ? nullptr : &it->second;
}

// ─── Main event dispatcher ────────────────────────────────────────────────────

bool XWaylandWM::handle_event(XEvent *ev) {
    // DEBUG_PRINT("[WM] handle_event type=%d win=0x%lx\n", ev->type, ev->xany.window);
    switch (ev->type) {
    case MapRequest:       DEBUG_PRINT("[WM] -> MapRequest\n");       return on_map_request(&ev->xmaprequest);
    case ConfigureRequest: DEBUG_PRINT("[WM] -> ConfigureRequest\n"); return on_configure_request(&ev->xconfigurerequest);
    case ReparentNotify:   DEBUG_PRINT("[WM] -> ReparentNotify\n");   return on_reparent_notify(&ev->xreparent);
    case UnmapNotify:      DEBUG_PRINT("[WM] -> UnmapNotify\n");      return on_unmap_notify(&ev->xunmap);
    case DestroyNotify:    DEBUG_PRINT("[WM] -> DestroyNotify\n");    return on_destroy_notify(&ev->xdestroywindow);
    default:
    DEBUG_PRINT("[WM] -> unhandled (type=%d)\n", ev->type);
        return false;
    }
}

// ─── MapRequest ───────────────────────────────────────────────────────────────
// Native plugins call XMapWindow; because we set SubstructureRedirect on the
// container, the X server redirects this to us as a MapRequest.  We MUST call
// XMapWindow ourselves or the window never appears.
bool XWaylandWM::on_map_request(XMapRequestEvent *ev) {
    DEBUG_PRINT("[WM] on_map_request win=0x%lx parent=0x%lx\n", ev->window, ev->parent);
    Window w = ev->window;

    // If we're not tracking this yet, start now
    if (!states_.count(w))
        track_window(w, ev->parent, true);

    auto &st = states_[w];

    // Honour the request: map the window
    XMapWindow(dpy_, w);
    DEBUG_PRINT("[WM] XMapWindow called for 0x%lx\n", ev->window);
    st.mapped = true;

    // Set WM_STATE = Normal (ICCCM §4.1.3.1)
    set_wm_state(w, WM_STATE_NORMAL);

    // Focus per ICCCM §4.1.7. WM_TAKE_FOCUS MUST carry a real timestamp,
    // never CurrentTime, or globally-active clients (input=False) ignore it
    // and then drop synthesized button events.
    if (st.take_focus)
        send_take_focus(w, get_server_time());
    // Globally-active windows advertise input=False AND rely on the WM.
    // If take-focus alone doesn't stick, force focus (the sway/wlroots fix).
    if (!st.input_hint || st.take_focus)
        XSetInputFocus(dpy_, w, RevertToParent, get_server_time());

    return true; // consumed
}

// ─── ConfigureRequest ─────────────────────────────────────────────────────────
// Native plugins send ConfigureRequest to move/resize themselves.
// We MUST respond with XConfigureWindow or they stall waiting.

bool XWaylandWM::on_configure_request(XConfigureRequestEvent *ev) {
    honour_configure_request(ev);
    return true;
}

void XWaylandWM::honour_configure_request(XConfigureRequestEvent *ev) {

    DEBUG_PRINT("[WM] honour_configure_request win=0x%lx x=%d y=%d w=%d h=%d mask=0x%lx\n",
                ev->window, ev->x, ev->y, ev->width, ev->height, ev->value_mask);
    Window w = ev->window;

    XWindowChanges wc{};
    wc.x            = ev->x;
    wc.y            = ev->y;
    wc.width        = ev->width;
    wc.height       = ev->height;
    wc.border_width = ev->border_width;
    wc.sibling      = ev->above;
    wc.stack_mode   = ev->detail;

    // Honour the client's requested position too. Plugins (esp. JUCE menus)
    // specify their own on-screen location via the ConfigureRequest / size
    // hints; stripping CWX|CWY is what made menus land center-screen.
    unsigned long mask = ev->value_mask;

    // If the client didn't put a position in the ConfigureRequest, fall back
    // to its WM_NORMAL_HINTS program/user-specified location (gravity Static).
    if (!(mask & (CWX | CWY))) {
        XSizeHints hints{}; long supplied = 0;
        if (XGetWMNormalHints(dpy_, w, &hints, &supplied) &&
            (hints.flags & (PPosition | USPosition))) {
            wc.x = hints.x;
            wc.y = hints.y;
            mask |= CWX | CWY;
        }
    }

    if (mask)
        XConfigureWindow(dpy_, w, mask, &wc);

    // Always send synthetic ConfigureNotify so client knows its actual position
    // This stops the client from looping waiting for acknowledgment
    XWindowAttributes attr{};
    if (XGetWindowAttributes(dpy_, w, &attr)) {
        XEvent synth{};
        synth.type                       = ConfigureNotify;
        synth.xconfigure.event           = w;
        synth.xconfigure.window          = w;
        synth.xconfigure.x               = attr.x;
        synth.xconfigure.y               = attr.y;
        synth.xconfigure.width           = attr.width;
        synth.xconfigure.height          = attr.height;
        synth.xconfigure.border_width    = attr.border_width;
        synth.xconfigure.above           = None;
        synth.xconfigure.override_redirect = attr.override_redirect;
        XSendEvent(dpy_, w, False, StructureNotifyMask, &synth);
    }
XFlush(dpy_);

    DEBUG_PRINT("[WM] XConfigureWindow done for 0x%lx\n", w);
    // Update our state
    if (states_.count(w)) {
        auto &st = states_[w];
        if (ev->value_mask & CWX)      { st.req_x = ev->x; st.placed_x = ev->x; }
        if (ev->value_mask & CWY)      { st.req_y = ev->y; st.placed_y = ev->y; }
        if (ev->value_mask & CWWidth)  { st.req_w = ev->width;  st.placed_w = ev->width; }
        if (ev->value_mask & CWHeight) { st.req_h = ev->height; st.placed_h = ev->height; }
    }

    // Notify our SWELL layer so it can resize the GTK widget / backing pixmap
    if (on_configure_applied)
        on_configure_applied(w, ev->x, ev->y, ev->width, ev->height);

    XFlush(dpy_);
}

// ─── ReparentNotify ───────────────────────────────────────────────────────────
// Sent when a window is reparented into our container. Track it and send a
// synthetic ConfigureNotify so native plugins learn their geometry and render.
bool XWaylandWM::on_reparent_notify(XReparentEvent *ev) {
    if (!containers_.count(ev->parent))
        return false;

    if (!states_.count(ev->window))
        track_window(ev->window, ev->parent, true);

    XWindowAttributes attr{};
    if (XGetWindowAttributes(dpy_, ev->window, &attr) == 0) return false;

    XEvent synth{};
    synth.type                         = ConfigureNotify;
    synth.xconfigure.event             = ev->window;
    synth.xconfigure.window            = ev->window;
    synth.xconfigure.x                 = ev->x;
    synth.xconfigure.y                 = ev->y;
    synth.xconfigure.width             = attr.width;
    synth.xconfigure.height            = attr.height;
    synth.xconfigure.border_width      = attr.border_width;
    synth.xconfigure.above             = None;
    synth.xconfigure.override_redirect = attr.override_redirect;
    XSendEvent(dpy_, ev->window, False, StructureNotifyMask, &synth);

    return false; // let the bridge also see this
}

// ─── UnmapNotify ──────────────────────────────────────────────────────────────
bool XWaylandWM::on_unmap_notify(XUnmapEvent *ev) {
    auto it = states_.find(ev->window);
    if (it == states_.end()) return false;

    it->second.mapped = false;
    set_wm_state(ev->window, WM_STATE_WITHDRAWN);

    return false; // let bridge also handle
}

// ─── DestroyNotify ────────────────────────────────────────────────────────────
bool XWaylandWM::on_destroy_notify(XDestroyWindowEvent *ev) {
    untrack_window(ev->window);
    return false;
}

// ─── WM_TAKE_FOCUS / WM_DELETE_WINDOW ────────────────────────────────────────
// Obtain a valid server timestamp (ICCCM forbids CurrentTime for WM_TAKE_FOCUS).
// Trick: do a zero-length append to a property on our support window and read
// the timestamp back off the resulting PropertyNotify.
Time XWaylandWM::get_server_time() {
    if (support_win_ == None) return CurrentTime; // last resort
    static Atom timestamp_atom = XInternAtom(dpy_, "_REAPERX_TIMESTAMP", False);
    XSelectInput(dpy_, support_win_, PropertyChangeMask);
    XChangeProperty(dpy_, support_win_, timestamp_atom, XA_ATOM, 32,
                    PropModeAppend, nullptr, 0);
    XFlush(dpy_);
    for (;;) {
        XEvent ev;
        XWindowEvent(dpy_, support_win_, PropertyChangeMask, &ev);
        if (ev.type == PropertyNotify &&
            ev.xproperty.atom == timestamp_atom) {
            return ev.xproperty.time;
        }
    }
}

void XWaylandWM::send_take_focus(Window w, Time t) {
    xwm_send_protocol_message(dpy_, w, atoms_.WM_TAKE_FOCUS, t);
}

void XWaylandWM::send_delete_window(Window w, Time t) {
    xwm_send_protocol_message(dpy_, w, atoms_.WM_DELETE_WINDOW, t);
}

// ─── WM_STATE property helper ─────────────────────────────────────────────────
// ICCCM §4.1.3.1  WM_STATE = { state, icon_window }
void XWaylandWM::set_wm_state(Window w, int state) {
    long data[2] = { state, None };
    XChangeProperty(dpy_, w,
                    atoms_.WM_STATE, atoms_.WM_STATE, 32,
                    PropModeReplace, (unsigned char*)data, 2);
}

// ─── Read WM_PROTOCOLS ────────────────────────────────────────────────────────
void XWaylandWM::read_wm_protocols(Window w, WMWindowState &st) {
    Atom *protocols = nullptr;
    int n = 0;
    if (XGetWMProtocols(dpy_, w, &protocols, &n)) {
        for (int i = 0; i < n; ++i) {
            if (protocols[i] == atoms_.WM_TAKE_FOCUS)
                st.take_focus = true;
            if (protocols[i] == atoms_.WM_DELETE_WINDOW)
                st.delete_window = true;
        }
        XFree(protocols);
    }
}

// ─── Read WM_HINTS ────────────────────────────────────────────────────────────
void XWaylandWM::read_wm_hints(Window w, WMWindowState &st) {
    XWMHints *hints = XGetWMHints(dpy_, w);
    if (!hints) return;
    if (hints->flags & InputHint)
        st.input_hint = (hints->input != 0);
    XFree(hints);
}

void xwm_redirect_container(Display *dpy, Window container) {
    XSelectInput(dpy, container,
                 SubstructureRedirectMask |
                 SubstructureNotifyMask   |
                 StructureNotifyMask      |
                 PropertyChangeMask);
    XFlush(dpy);
}

void xwm_send_reparent_synthetic(Display *dpy, Window client,
                                  Window new_parent, int x, int y) {
    (void)new_parent;
    XWindowAttributes attr{};
    if (!XGetWindowAttributes(dpy, client, &attr)) return;

    XEvent ev{};
    ev.type                         = ConfigureNotify;
    ev.xconfigure.event             = client;
    ev.xconfigure.window            = client;
    ev.xconfigure.x                 = x;
    ev.xconfigure.y                 = y;
    ev.xconfigure.width             = attr.width;
    ev.xconfigure.height            = attr.height;
    ev.xconfigure.border_width      = attr.border_width;
    ev.xconfigure.above             = None;
    ev.xconfigure.override_redirect = False;
    XSendEvent(dpy, client, False, StructureNotifyMask, &ev);
    XFlush(dpy);
}

void xwm_send_protocol_message(Display *dpy, Window w, Atom protocol, Time t) {
    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    XEvent ev{};
    ev.type                 = ClientMessage;
    ev.xclient.window       = w;
    ev.xclient.message_type = wm_protocols;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = (long)protocol;
    ev.xclient.data.l[1]    = (long)t;
    XSendEvent(dpy, w, False, NoEventMask, &ev);
    XFlush(dpy);
}

bool is_window_from_owned_plugin(Display *dpy, Window org_win, Window gui_win) {
    // Check if this window has the same PID as our plugin
    Atom atom_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    pid_t window_pid = 0;
    if (XGetWindowProperty(dpy, org_win, atom_pid, 0, 1, False, XA_CARDINAL,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success) {
        if (prop && nitems > 0) {
            window_pid = *((pid_t*)prop);
            XFree(prop);
        }
    }

    // Get our plugin's PID (from main_plugin_gui)
    pid_t our_pid = 0;
    if (XGetWindowProperty(dpy, gui_win, atom_pid, 0, 1, False, XA_CARDINAL,
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

bool classify_popup(Display *dpy, Window win, XWindowAttributes *attr) {
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
