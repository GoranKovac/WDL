// xwayland-bridge-wm.h
// Minimal ICCCM/EWMH window manager layer for the XWayland bridge.
// Handles: grab management, WM_PROTOCOLS, ConfigureRequest, focus protocol,
// MapRequest/ReparentNotify so native X11 LV2/VST plugins render correctly.

#pragma once
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <map>
#include <vector>
#include <functional>

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
    // Xwayland
    Atom _XWAYLAND_ALLOW_COMMITS;
};

// WM_STATE values (ICCCM §4.1.3.1)
#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL    1
#define WM_STATE_ICONIC    3

// ─── Per-window WM state ──────────────────────────────────────────────────────
struct WMWindowState {
    bool input_hint;         // from WM_HINTS.input field
    bool take_focus;         // supports WM_TAKE_FOCUS protocol
};

// ─── The minimal WM ──────────────────────────────────────────────────────────
class XWaylandWM {
public:
    explicit XWaylandWM(Display *dpy);

    // One-shot bring-up: spawn Xwayland on the given display, install the X error
    // handler, open the WM connection, construct + announce the WM, and attach a
    // GIO watch that pumps events on the GLib main loop. Returns the global WM
    // instance (also stored in g_wm) or nullptr on failure. Infrastructure that
    // has nothing to do with the bridge lives here.
    static XWaylandWM* init_bridge_wm(const char *display_name = ":10");

    // Call once after creating your root/parent windows to announce ourselves
    // as the WM (needed for native plugins that check _NET_SUPPORTING_WM_CHECK)
    void announce_wm(Window support_win);

    // ── Event dispatch ────────────────────────────────────────────────────────
    // Feed every XEvent from your poll loop into here BEFORE your own handling.
    // Returns true if the event was fully consumed by the WM layer.
    bool handle_event(XEvent *ev);

    void track_window(Window w);
    void untrack_window(Window w);

    void set_active_window(Window window);
    Time get_server_time();
    // Send WM_TAKE_FOCUS to a window (ICCCM §4.1.7).
    void send_take_focus(Window w, Time t = CurrentTime);
    // ── ConfigureRequest handling ─────────────────────────────────────────────
    // Honours a client ConfigureRequest and updates our bookkeeping.
    void honour_configure_request(XConfigureRequestEvent *ev);

    // ── Drag-and-drop OUT of a plugin (XDND catcher) ──────────────────────────
    // Catches XDND drags a Wine/native plugin starts that have no compositor-
    // visible target to drop onto (REAPER is a native Wayland client and has no
    // X11 window of its own). Pure Xlib/XFixes protocol handling -- see
    // xwayland-bridge-wm.cpp for the full rationale. Call dnd_init() once the
    // WM's X connection is up; handle_event() then folds the protocol handling
    // in automatically. The rest of this API is how a toolkit-specific caller
    // pulls the result out to drive its own native drag.
    void dnd_init();

    // True once a drag payload has been fetched via XConvertSelection and the
    // source's button is still held -- the only time a Wayland-side drag can be
    // started with a valid input serial. Poll from a real input event handler
    // (e.g. a GTK motion callback) and use dnd_take_pending_path() to consume it.
    bool dnd_has_pending() const { return dnd_pending_; }

    // Consumes the pending drag payload, decoding a "file://…" URI-list entry
    // into a plain path ("/a/b c.mid"). Returns false (and leaves *out empty)
    // if nothing was pending. Caller still owns starting its own native drag.
    bool dnd_take_pending_path(char *out, size_t outsz);

    // A caller's native drag call takes the input capture for its duration, so
    // the XDND source's button-release never reaches the WM's X display -- call
    // this with the source plugin's own display connection once that native
    // drag call returns, or yabridge's XDND loop is left waiting forever for a
    // release that already happened elsewhere.
    void dnd_release_source_button(Display *dpy);

    // Window id of the invisible XDND proxy. Callers should ignore Map/Unmap
    // for this window rather than mistake it for real plugin content.
    Window dnd_catcher_window() const { return dnd_catcher_; }

    // Fired for every event AFTER the WM layer has had a look, so the bridge can
    // do its own per-event work (captures, popups, modals, damage). The WM does
    // not consume plugin/popup content events; the bridge owns that.
    std::function<void(XEvent* /*ev*/)> on_unhandled_event;

private:
    void init_atoms();
    void set_wm_state(Window w, int state);
    void read_wm_protocols(Window w, WMWindowState &st);
    void read_wm_hints(Window w, WMWindowState &st);
    void disallow_commits(Window w);

    bool on_map_request(XMapRequestEvent *ev);
    bool on_create_notify(XCreateWindowEvent *ev);
    bool on_configure_request(XConfigureRequestEvent *ev);
    bool on_reparent_notify(XReparentEvent *ev);
    bool on_unmap_notify(XUnmapEvent *ev);
    bool on_destroy_notify(XDestroyWindowEvent *ev);

    // DND catcher internals (see xwayland-bridge-wm.cpp for the protocol walk).
    bool dnd_handle_event(XEvent *ev);
    void dnd_catcher_show(bool on);
    void dnd_send_status(Window src, bool accept);
    void dnd_send_finished(Window src, bool accepted);
    void dnd_reset_payload();

    Display    *dpy_;
    WMAtoms     atoms_;
    std::map<Window, WMWindowState> states_;
    Window      support_win_ = None;
    Window      active_window_ = ~0UL;

    // ── DND catcher state ───────────────────────────────────────────────────
    Window   dnd_catcher_        = 0;
    int      dnd_xfixes_evt_base_ = -1;
    Window   dnd_source_         = 0;    // yabridge's drag source window
    bool     dnd_requested_      = false; // selection conversion already asked for
    // Payload, fetched while the button is still down. Plain buffer rather than
    // std::string: some callers' headers define min/max as macros, so pulling in
    // <string> alongside them is risky -- keep this layer header-light.
    char     dnd_uri_[8192]      = {};
    bool     dnd_have_uri_       = false;
    bool     dnd_pending_        = false;
    Atom     a_XdndSelection_ = None, a_XdndAware_ = None, a_XdndEnter_ = None, a_XdndPosition_ = None;
    Atom     a_XdndStatus_ = None, a_XdndDrop_ = None, a_XdndFinished_ = None, a_XdndLeave_ = None;
    Atom     a_XdndActionCopy_ = None, a_uri_list_ = None, a_dnd_prop_ = None;
};

void xwm_send_protocol_message(Display *dpy, Window w, Atom protocol, Time t);

bool is_window_from_owned_plugin(Display *dpy, Window org_win, Window gui_win);

bool classify_popup(Display *dpy, Window win, XWindowAttributes *attr);
