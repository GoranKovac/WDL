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
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xcomposite.h>
#include <cstring>
#include <cstdio>
#include <thread>
#include <atomic>
#include <algorithm>

// ─── Construction ─────────────────────────────────────────────────────────────
Window active_popup_ = None;
XWaylandWM::XWaylandWM(Display *dpy) : dpy_(dpy) {
    init_atoms();
}

XWaylandWM::~XWaylandWM() {
    release_grab();
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

void XWaylandWM::start_event_loop()
{
    running_ = true;
    event_thread_ = std::thread([this]() {
        DEBUG_PRINT("[WM] event loop thread started\n");
        while (running_) {
            // Block until an event arrives — no polling, no CPU waste
            XEvent ev;
            XNextEvent(dpy_, &ev);
            DEBUG_PRINT("[WM] thread got event type=%d win=0x%lx\n", ev.type, ev.xany.window);
            handle_event(&ev);
        }
        DEBUG_PRINT("[WM] event loop thread stopped\n");
    });
}

void XWaylandWM::stop_event_loop()
{
    running_ = false;
    // Wake the thread if it's blocked on XNextEvent
    // Send a harmless ClientMessage to ourselves
    XClientMessageEvent ev{};
    ev.type         = ClientMessage;
    ev.window       = support_win_;
    ev.message_type = XInternAtom(dpy_, "WM_PROTOCOLS", False);
    ev.format       = 32;
    XSendEvent(dpy_, support_win_, False, NoEventMask, (XEvent*)&ev);
    XFlush(dpy_);
    if (event_thread_.joinable())
        event_thread_.join();
}

// ─── Container registration ───────────────────────────────────────────────────

void XWaylandWM::register_container(Window container, bool is_native) {
    containers_.insert(container);
    // SubstructureRedirect lets us intercept MapRequest/ConfigureRequest
    xwm_redirect_container(dpy_, container);
    (void)is_native; // per-child flag set in track_window
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
    DEBUG_PRINT("[WM] handle_event type=%d win=0x%lx\n", ev->type, ev->xany.window);
    switch (ev->type) {
    case MapRequest:       DEBUG_PRINT("[WM] -> MapRequest\n");       return on_map_request(&ev->xmaprequest);
    case ConfigureRequest: DEBUG_PRINT("[WM] -> ConfigureRequest\n"); return on_configure_request(&ev->xconfigurerequest);
    case ReparentNotify:   DEBUG_PRINT("[WM] -> ReparentNotify\n");   return on_reparent_notify(&ev->xreparent);
    case UnmapNotify:      DEBUG_PRINT("[WM] -> UnmapNotify\n");      return on_unmap_notify(&ev->xunmap);
    case DestroyNotify:    DEBUG_PRINT("[WM] -> DestroyNotify\n");    return on_destroy_notify(&ev->xdestroywindow);
    case ClientMessage:    DEBUG_PRINT("[WM] -> ClientMessage\n");    return on_client_message(&ev->xclient);
    // case ButtonPress:      DEBUG_PRINT("[WM] -> ButtonPress\n");      return on_button_press(&ev->xbutton);
    case PropertyNotify:   DEBUG_PRINT("[WM] -> PropertyNotify\n");   return on_property_notify(&ev->xproperty);
    case CreateNotify:     DEBUG_PRINT("[WM] -> CreateNotify\n");     return on_create_notify(&ev->xcreatewindow);
    case MapNotify:        DEBUG_PRINT("[WM] -> MapNotify\n");        return on_map_notify(&ev->xmap);
    case KeyPress:
        DEBUG_PRINT("[WM] KeyPress win=0x%lx keycode=%u state=0x%x time=%lu\n",
                    ev->xkey.window,
                    ev->xkey.keycode,
                    ev->xkey.state,
                    ev->xkey.time);
        return false;

    case KeyRelease:
        DEBUG_PRINT("[WM] KeyRelease win=0x%lx keycode=%u state=0x%x time=%lu\n",
                    ev->xkey.window,
                    ev->xkey.keycode,
                    ev->xkey.state,
                    ev->xkey.time);
        return false;
    default:
    DEBUG_PRINT("[WM] -> unhandled (type=%d)\n", ev->type);
        return false;
    }
}
//NOTE: ADD POPUPS!!!!
bool XWaylandWM::on_create_notify(XCreateWindowEvent *ev)
{
    XWindowAttributes attr{};

    if (!XGetWindowAttributes(dpy_, ev->window, &attr))
        return false;

    DEBUG_PRINT(
        "[WM] CreateNotify win=0x%lx parent=0x%lx override_redirect=%d geom=%dx%d+%d+%d\n",
        ev->window,
        ev->parent,
        attr.override_redirect,
        attr.width,
        attr.height,
        attr.x,
        attr.y);

    if (attr.override_redirect) {
        DEBUG_PRINT("[WM] override-redirect popup candidate: 0x%lx\n",
                    ev->window);

        active_popup_ = ev->window;

        // if (!states_.count(ev->window)) {
        //     track_window(ev->window, ev->parent, true);
        // }
        //
        // XSelectInput(dpy_,
        //              ev->window,
        //              StructureNotifyMask |
        //              PropertyChangeMask);
        //
        // //
        // // Catch outside clicks.
        // //
        // int result = XGrabPointer(
        //     dpy_,
        //     support_win_,
        //     True,                    // owner_events
        //     ButtonPressMask |
        //     ButtonReleaseMask,
        //     GrabModeAsync,
        //     GrabModeAsync,
        //     None,
        //     None,
        //     CurrentTime);
        //
        // DEBUG_PRINT("[WM] outside-click grab result=%d\n", result);
        //
        // XFlush(dpy_);
    }

    return false;
}

bool XWaylandWM::on_map_notify(XMapEvent *ev)
{
    XWindowAttributes attr{};

    if (!XGetWindowAttributes(dpy_, ev->window, &attr))
        return false;

    DEBUG_PRINT("[WM] MapNotify win=0x%lx override_redirect=%d\n",
                ev->window,
                attr.override_redirect);

    if (attr.override_redirect) {
        DEBUG_PRINT("[WM] removing popup keyboard grab\n");

        XUngrabKeyboard(dpy_, CurrentTime);
        XFlush(dpy_);

        // active_popup_ = ev->window;
    }

    return false;
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
// Sent when a window is reparented into our container.

bool XWaylandWM::on_reparent_notify(XReparentEvent *ev) {
    if (!containers_.count(ev->parent))
        return false;

    if (!states_.count(ev->window))
        track_window(ev->window, ev->parent, true);

    // Some native plugins need a synthetic ConfigureNotify after reparent
    // so they know their new position within the parent. Send it.
    XWindowAttributes attr{};
    if (XGetWindowAttributes(dpy_, ev->window, &attr) == 0) return false;

    XEvent synth{};
    synth.type                    = ConfigureNotify;
    synth.xconfigure.event        = ev->window;
    synth.xconfigure.window       = ev->window;
    synth.xconfigure.x            = ev->x;
    synth.xconfigure.y            = ev->y;
    synth.xconfigure.width        = attr.width;
    synth.xconfigure.height       = attr.height;
    synth.xconfigure.border_width = attr.border_width;
    synth.xconfigure.above        = None;
    synth.xconfigure.override_redirect = attr.override_redirect;
    XSendEvent(dpy_, ev->window, False, StructureNotifyMask, &synth);

    return false; // let the rest of the bridge also see this
}

// ─── UnmapNotify ──────────────────────────────────────────────────────────────

bool XWaylandWM::on_unmap_notify(XUnmapEvent *ev) {
    auto it = states_.find(ev->window);
    if (it == states_.end()) return false;

    it->second.mapped = false;
    set_wm_state(ev->window, WM_STATE_WITHDRAWN);

    // If this window owned the grab, release it
    if (grab_.grab_window == ev->window)
        release_grab();

    return false; // let bridge also handle
}

// ─── DestroyNotify ────────────────────────────────────────────────────────────

bool XWaylandWM::on_destroy_notify(XDestroyWindowEvent *ev) {
    if (active_popup_ == ev->window)
        active_popup_ = None;
    untrack_window(ev->window);
    if (grab_.grab_window == ev->window)
        release_grab();
    return false;
}

// ─── ClientMessage ────────────────────────────────────────────────────────────

bool XWaylandWM::on_client_message(XClientMessageEvent *ev) {
    // _NET_WM_STATE change request (e.g. plugin wants to go modal)
    if ((Atom)ev->message_type == atoms_._NET_WM_STATE) {
        // ev->data.l[0] = 0 remove, 1 add, 2 toggle
        // ev->data.l[1..2] = atoms to change
        // We update our state but don't veto — the WM would normally do
        // decoration changes here, which we skip.
        if (states_.count(ev->window)) {
            long action = ev->data.l[0];
            for (int i = 1; i <= 2; ++i) {
                Atom a = (Atom)ev->data.l[i];
                if (a == atoms_._NET_WM_STATE_MODAL) {
                    if (action == 1 || action == 2)
                        states_[ev->window].is_modal = true;
                    else
                        states_[ev->window].is_modal = false;
                }
            }
        }
        return false;
    }

    // WM_CHANGE_STATE iconify request — we just ignore
    if ((Atom)ev->message_type == atoms_.WM_CHANGE_STATE)
        return true;

    // _NET_ACTIVE_WINDOW request
    if ((Atom)ev->message_type == atoms_._NET_ACTIVE_WINDOW) {
        Window w = ev->window;
        if (states_.count(w) && states_[w].take_focus)
            send_take_focus(w, (Time)ev->data.l[1]);
        return true;
    }

    return false;
}

// ─── ButtonPress — the freeze fix ────────────────────────────────────────────
// With owner_events=True the grab routes all button events through here.
// If the click is outside every tracked popup/modal, we dismiss them and
// release the grab, preventing the deadlock.

// bool XWaylandWM::on_button_press(XButtonEvent *ev)
// {
//     if (active_popup_ == None)
//         return false;
//
//     DEBUG_PRINT("[WM] popup ButtonPress win=0x%lx popup=0x%lx root=(%d,%d)\n",
//                 ev->window,
//                 active_popup_,
//                 ev->x_root,
//                 ev->y_root);
//
//     bool inside = false;
//
//     XWindowAttributes attr{};
//     if (XGetWindowAttributes(dpy_, active_popup_, &attr)) {
//
//         int x = 0;
//         int y = 0;
//         Window child = None;
//
//         XTranslateCoordinates(dpy_,
//                               DefaultRootWindow(dpy_),
//                               active_popup_,
//                               ev->x_root,
//                               ev->y_root,
//                               &x,
//                               &y,
//                               &child);
//
//         inside =
//             x >= 0 &&
//             y >= 0 &&
//             x < attr.width &&
//             y < attr.height;
//     }
//
//     if (!inside) {
//         DEBUG_PRINT("[WM] outside popup -> releasing X11 grab\n");
//
//         XUngrabPointer(dpy_, ev->time);
//         XUngrabKeyboard(dpy_, ev->time);
//
//         // Required for some native X11 popup implementations
//         KeyCode esc = XKeysymToKeycode(dpy_, XK_Escape);
//         XTestFakeKeyEvent(dpy_, esc, True, CurrentTime);
//         XTestFakeKeyEvent(dpy_, esc, False, CurrentTime);
//
//         XFlush(dpy_);
//
//         active_popup_ = None;
//     }
//
//     return false;
// }
//
// bool XWaylandWM::handle_outside_click(Window /*clicked*/, int /*x*/, int /*y*/, Time t) {
//     if (grab_.owner == GRAB_NONE) return false;
//
//     Window dismissed = grab_.grab_window;
//     release_grab(t);
//
//     if (on_popup_dismissed)
//         on_popup_dismissed(dismissed);
//
//     return true;
// }

// ─── PropertyNotify — re-read hints when client updates them ─────────────────

bool XWaylandWM::on_property_notify(XPropertyEvent *ev) {
    if (!states_.count(ev->window)) return false;
    auto &st = states_[ev->window];

    if (ev->atom == atoms_.WM_PROTOCOLS)
        read_wm_protocols(ev->window, st);
    else if (ev->atom == atoms_.WM_HINTS)
        read_wm_hints(ev->window, st);
    else if (ev->atom == atoms_.WM_TRANSIENT_FOR) {
        Window trans = None;
        XGetTransientForHint(dpy_, ev->window, &trans);
        st.transient_for = trans;
    }
    return false;
}

// ─── Focus ────────────────────────────────────────────────────────────────────

void XWaylandWM::on_gtk_focus(GtkWidget * /*widget*/, Window x11_win) {
    if (!states_.count(x11_win)) return;
    auto &st = states_[x11_win];
    Time t = get_server_time();
    if (st.take_focus)
        send_take_focus(x11_win, t);
    // Force focus for globally-active (input=False) and as a robust fallback.
    XSetInputFocus(dpy_, x11_win, RevertToParent, t);
}

void XWaylandWM::on_gtk_unfocus(GtkWidget * /*widget*/) {
    // Nothing to do — plugin retains focus within its container
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

// ─── Grab management — the core of the freeze fix ─────────────────────────────
//
// owner_events = True  →  pointer events go to the window they're in,
//                          not just to grab_window.  This is what lets the
//                          user click in other Reaper windows without freezing.
// event_mask           →  events we want even when outside any of our windows.
// confine_to = None    →  don't trap the cursor.
// cursor = None        →  keep whatever cursor the window sets.

bool XWaylandWM::acquire_grab(GrabOwner who, Window x11_win, GtkWidget *gtk_w, Time t) {
    if (grab_.owner != GRAB_NONE)
        release_grab(t);

    int ptr_result = XGrabPointer(
        dpy_, x11_win,
        True,                       // owner_events = True  ← KEY
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
        EnterWindowMask | LeaveWindowMask,
        GrabModeAsync, GrabModeAsync,
        None,                       // confine_to
        None,                       // cursor
        t
    );
    if (ptr_result != GrabSuccess) {
        fprintf(stderr, "[XWaylandWM] XGrabPointer failed: %d\n", ptr_result);
        return false;
    }

    int kbd_result = XGrabKeyboard(
        dpy_, x11_win,
        True,                       // owner_events = True  ← KEY
        GrabModeAsync, GrabModeAsync,
        t
    );
    if (kbd_result != GrabSuccess) {
        // Keyboard grab failure is non-fatal; pointer grab is more important
        fprintf(stderr, "[XWaylandWM] XGrabKeyboard failed: %d (non-fatal)\n", kbd_result);
    }

    grab_.owner            = who;
    grab_.grab_window      = x11_win;
    grab_.gtk_widget       = gtk_w;
    grab_.pointer_grabbed  = (ptr_result == GrabSuccess);
    grab_.keyboard_grabbed = (kbd_result == GrabSuccess);
    grab_.grab_time        = t;

    XFlush(dpy_);
    return true;
}

void XWaylandWM::release_grab(Time t) {
    if (grab_.pointer_grabbed) {
        XUngrabPointer(dpy_, t);
        grab_.pointer_grabbed = false;
    }
    if (grab_.keyboard_grabbed) {
        XUngrabKeyboard(dpy_, t);
        grab_.keyboard_grabbed = false;
    }
    grab_.owner       = GRAB_NONE;
    grab_.grab_window = None;
    grab_.gtk_widget  = nullptr;
    XFlush(dpy_);
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

// ─────────────────────────────────────────────────────────────────────────────
// Free helpers
// ─────────────────────────────────────────────────────────────────────────────

void xwm_redirect_container(Display *dpy, Window container) {
    // SubstructureRedirectMask lets us receive MapRequest/ConfigureRequest
    // for direct children of `container`.
    // SubstructureNotifyMask lets us receive CreateNotify/DestroyNotify etc.
    XSelectInput(dpy, container,
                 SubstructureRedirectMask |
                 SubstructureNotifyMask   |
                 StructureNotifyMask      |
                 PropertyChangeMask);
    XFlush(dpy);
}

void xwm_send_reparent_synthetic(Display *dpy, Window client,
                                  Window new_parent, int x, int y) {
    // After XReparentWindow the X server sends a ReparentNotify to the client,
    // but some ICCCM clients also expect a synthetic ConfigureNotify with the
    // new geometry relative to the new parent so they can redraw.
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
