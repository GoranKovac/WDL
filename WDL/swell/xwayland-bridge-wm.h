// xwayland-bridge-wm.h
// Minimal ICCCM/EWMH window manager layer for the XWayland bridge.
// Handles: grab management, WM_PROTOCOLS, ConfigureRequest, focus protocol,
// MapRequest/ReparentNotify so native X11 LV2/VST plugins render correctly.

#pragma once
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>

// ─── Atoms we manage ─────────────────────────────────────────────────────────
struct WMAtoms {
    // ICCCM
    Atom WM_PROTOCOLS;
    Atom WM_DELETE_WINDOW;
    Atom WM_TAKE_FOCUS;
    Atom WM_STATE;
    Atom WM_CHANGE_STATE;
    Atom WM_HINTS;
    Atom WM_NORMAL_HINTS;
    Atom WM_TRANSIENT_FOR;
    Atom WM_NAME;
    Atom WM_CLASS;
    // EWMH
    Atom _NET_WM_STATE;
    Atom _NET_WM_STATE_MODAL;
    Atom _NET_WM_STATE_ABOVE;
    Atom _NET_ACTIVE_WINDOW;
    Atom _NET_WM_WINDOW_TYPE;
    Atom _NET_WM_WINDOW_TYPE_DIALOG;
    Atom _NET_WM_WINDOW_TYPE_POPUP_MENU;
    Atom _NET_WM_WINDOW_TYPE_TOOLTIP;
    Atom _NET_WM_WINDOW_TYPE_NORMAL;
    Atom _NET_SUPPORTING_WM_CHECK;
    Atom _NET_WM_NAME;
    Atom _NET_SUPPORTED;
    // Composite
    Atom _NET_WM_BYPASS_COMPOSITOR;
};

// WM_STATE values (ICCCM §4.1.3.1)
#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL    1
#define WM_STATE_ICONIC    3

// ─── Per-window WM state ──────────────────────────────────────────────────────
struct WMWindowState {
    Window      xid;
    Window      transient_for;      // WM_TRANSIENT_FOR hint
    bool        mapped;
    bool        input_hint;         // from WM_HINTS.input field
    bool        take_focus;         // supports WM_TAKE_FOCUS protocol
    bool        delete_window;      // supports WM_DELETE_WINDOW protocol
    bool        is_modal;
    bool        is_popup;
    bool        is_native_plugin;   // non-Wine native X11 plugin
    int         configure_serial;   // last ConfigureRequest serial honoured
    // geometry as requested by client
    int         req_x, req_y, req_w, req_h;
    // geometry as placed by us
    int         placed_x, placed_y, placed_w, placed_h;
};

// ─── Grab state machine ───────────────────────────────────────────────────────
enum GrabOwner { GRAB_NONE, GRAB_PLUGIN, GRAB_MODAL, GRAB_POPUP };

struct GrabState {
    GrabOwner   owner       = GRAB_NONE;
    Window      grab_window = None;     // X11 window that owns the grab
    GtkWidget  *gtk_widget  = nullptr;  // corresponding GTK widget
    bool        pointer_grabbed = false;
    bool        keyboard_grabbed = false;
    Time        grab_time   = CurrentTime;
};

// ─── The minimal WM ──────────────────────────────────────────────────────────
class XWaylandWM {
public:
    explicit XWaylandWM(Display *dpy);
    ~XWaylandWM();
    void start_event_loop();
    void stop_event_loop();

    // Call once after creating your root/parent windows to announce ourselves
    // as the WM (needed for native plugins that check _NET_SUPPORTING_WM_CHECK)
    void announce_wm(Window support_win);

    // ── Event dispatch ────────────────────────────────────────────────────────
    // Feed every XEvent from your poll loop into here BEFORE your own handling.
    // Returns true if the event was fully consumed by the WM layer.
    bool handle_event(XEvent *ev);

    // ── Window registration ───────────────────────────────────────────────────
    // Register a plugin container window so we manage its children.
    void register_container(Window container, bool is_native_plugin = false);
    void unregister_container(Window container);

    // Register a specific child (called from your MapNotify / ReparentNotify).
    void track_window(Window w, Window parent, bool is_native_plugin = false);
    void untrack_window(Window w);

    // ── Focus management ──────────────────────────────────────────────────────
    // Call when a GTK widget gains focus.
    void on_gtk_focus(GtkWidget *widget, Window x11_win);
    // Call when a GTK widget loses focus.
    void on_gtk_unfocus(GtkWidget *widget);
    Time get_server_time();
    // Send WM_TAKE_FOCUS to a window (ICCCM §4.1.7).
    void send_take_focus(Window w, Time t = CurrentTime);
    // Send WM_DELETE_WINDOW (ICCCM §4.2.8).
    void send_delete_window(Window w, Time t = CurrentTime);

    // ── Grab management ───────────────────────────────────────────────────────
    // Acquire an owner_events grab on behalf of a popup or modal.
    // owner_events=true means events still go to the correct window — this is
    // what lets us click outside a popup without freezing.
    bool acquire_grab(GrabOwner who, Window x11_win, GtkWidget *gtk_w, Time t);
    void release_grab(Time t = CurrentTime);
    // Called by your button-press handler to check if we should dismiss a popup.
    // Returns true if the click was outside all tracked popups/modals.
    bool handle_outside_click(Window clicked_win, int x_root, int y_root, Time t);

    // ── ConfigureRequest handling ─────────────────────────────────────────────
    // Honours a client ConfigureRequest and updates our bookkeeping.
    void honour_configure_request(XConfigureRequestEvent *ev);

    // ── Utility ───────────────────────────────────────────────────────────────
    WMAtoms& atoms() { return atoms_; }
    const WMWindowState* window_state(Window w) const;
    bool is_tracked(Window w) const { return states_.count(w) > 0; }

    // Callbacks — wire these up after construction
    std::function<void(Window /*dismissed*/)> on_popup_dismissed;
    std::function<void(Window /*w*/, int x, int y, int w2, int h)> on_configure_applied;
    Window active_popup_ = None;

private:
    void init_atoms();
    void set_wm_state(Window w, int state);
    void read_wm_protocols(Window w, WMWindowState &st);
    void read_wm_hints(Window w, WMWindowState &st);

    bool on_map_request(XMapRequestEvent *ev);
    bool on_map_notify(XMapEvent *ev);
    bool on_create_notify(XCreateWindowEvent *ev);
    bool on_configure_request(XConfigureRequestEvent *ev);
    bool on_reparent_notify(XReparentEvent *ev);
    bool on_unmap_notify(XUnmapEvent *ev);
    bool on_destroy_notify(XDestroyWindowEvent *ev);
    bool on_client_message(XClientMessageEvent *ev);
    bool on_button_press(XButtonEvent *ev);
    bool on_property_notify(XPropertyEvent *ev);

    Display    *dpy_;
    WMAtoms     atoms_;
    std::map<Window, WMWindowState> states_;
    std::set<Window> containers_;
    GrabState   grab_;
    Window      support_win_ = None;
    std::thread event_thread_;
    std::atomic<bool> running_{false};
};

// ─── Free helpers used by swell-generic-gdk.cpp ──────────────────────────────

// Subscribe the WM to SubstructureRedirect on a container so we receive
// MapRequest / ConfigureRequest from native plugin children.
void xwm_redirect_container(Display *dpy, Window container);

// After XReparentWindow, send the synthetic ReparentNotify + ConfigureNotify
// that ICCCM-compliant clients expect from their new WM parent.
void xwm_send_reparent_synthetic(Display *dpy, Window client, Window new_parent,
                                  int x, int y);

// Send a WM_PROTOCOLS ClientMessage.
void xwm_send_protocol_message(Display *dpy, Window w, Atom protocol, Time t);
