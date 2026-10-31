// xwayland-bridge-wm.h
// Minimal ICCCM/EWMH window manager layer for the XWayland bridge.
// Handles: grab management, WM_PROTOCOLS, ConfigureRequest, focus protocol,
// MapRequest/ReparentNotify so native X11 LV2/VST plugins render correctly.

#pragma once
#include <map>
#include <vector>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>

// ─── Atoms we manage ─────────────────────────────────────────────────────────
struct WMAtoms : xcb_ewmh_connection_t {
    xcb_atom_t WM_DELETE_WINDOW, WM_TAKE_FOCUS, WM_STATE;
    xcb_atom_t _XWAYLAND_ALLOW_COMMITS, _REAPERX_TIMESTAMP;
    xcb_atom_t XdndSelection, XdndAware, XdndEnter, XdndPosition;
    xcb_atom_t XdndStatus, XdndDrop, XdndFinished, XdndLeave, XdndActionCopy;
    xcb_atom_t uri_list, dnd_data, motif_hints;
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
    explicit XWaylandWM(xcb_connection_t *connection);
    xcb_connection_t *connection() const { return connection_; }
    const WMAtoms &atoms() const { return atoms_; }
    bool owns_window(xcb_window_t window, xcb_window_t gui);
    enum WindowType { NORMAL, POPUP, DRAG_ICON };
    WindowType window_type(xcb_window_t window, bool override_redirect);

    // One-shot bring-up: spawn Xwayland on a free display, open the WM
    // connection, construct + announce the WM, and attach a
    // GIO watch that pumps events on the GLib main loop. Returns the global WM
    // instance (also stored in g_wm) or nullptr on failure. Infrastructure that
    // has nothing to do with the bridge lives here.
    static XWaylandWM* init_bridge_wm();
    static const char *display_name();

    // Call once after creating your root/parent windows to announce ourselves
    // as the WM (needed for native plugins that check _NET_SUPPORTING_WM_CHECK)
    void announce_wm(xcb_window_t support_win);

    // ── Event dispatch ────────────────────────────────────────────────────────
    // Feed every X event from your poll loop into here BEFORE your own handling.
    // Returns true if the event was fully consumed by the WM layer.
    bool handle_event(xcb_generic_event_t *ev);

    void track_window(xcb_window_t w);
    void untrack_window(xcb_window_t w);

    void set_active_window(xcb_window_t window);
    // Send WM_TAKE_FOCUS to a window (ICCCM §4.1.7).
    void send_take_focus(xcb_window_t w, xcb_timestamp_t t);
    // ── ConfigureRequest handling ─────────────────────────────────────────────
    // Honours a client ConfigureRequest and updates our bookkeeping.
    void honour_configure_request(xcb_configure_request_event_t *ev);

    // ── Drag-and-drop OUT of a plugin (XDND catcher) ──────────────────────────
    // Catches XDND drags a Wine/native plugin starts that have no compositor-
    // visible target to drop onto (REAPER is a native Wayland client and has no
    // X11 window of its own). Pure XCB/XFixes protocol handling -- see
    // xwayland-bridge-wm.cpp for the full rationale. Call dnd_init() once the
    // WM's X connection is up; handle_event() then folds the protocol handling
    // in automatically. The rest of this API is how a toolkit-specific caller
    // pulls the result out to drive its own native drag.
    void dnd_init();

    // True once a drag payload has been fetched via a selection conversion and the
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
    void dnd_release_source_button(xcb_connection_t *connection);

    // Window id of the invisible XDND proxy. Callers should ignore Map/Unmap
    // for this window rather than mistake it for real plugin content.
    xcb_window_t dnd_catcher_window() const { return dnd_catcher_; }

    // Fired for every event AFTER the WM layer has had a look, so the bridge can
    // do its own per-event work (captures, popups, modals, damage). The WM does
    // not consume plugin/popup content events; the bridge owns that.
    void (*on_unhandled_event)(xcb_generic_event_t *ev) = nullptr;

private:
    void init_atoms();
    void set_wm_state(xcb_window_t w, int state);
    void read_wm_protocols(xcb_window_t w, WMWindowState &st);
    void read_wm_hints(xcb_window_t w, WMWindowState &st);
    void disallow_commits(xcb_window_t w);

    bool on_map_request(xcb_map_request_event_t *ev);
    bool on_create_notify(xcb_create_notify_event_t *ev);
    bool on_configure_request(xcb_configure_request_event_t *ev);
    bool on_reparent_notify(xcb_reparent_notify_event_t *ev);
    bool on_unmap_notify(xcb_unmap_notify_event_t *ev);
    bool on_destroy_notify(xcb_destroy_notify_event_t *ev);

    // DND catcher internals (see xwayland-bridge-wm.cpp for the protocol walk).
    bool dnd_handle_event(xcb_generic_event_t *ev);
    void dnd_catcher_show(bool on);
    void dnd_send_status(xcb_window_t src, bool accept);
    void dnd_send_finished(xcb_window_t src, bool accepted);
    void dnd_reset_payload();

    xcb_connection_t *connection_;
    xcb_window_t root_;
    WMAtoms atoms_ = {};
    xcb_window_t pending_focus_ = XCB_NONE;
    uint16_t focus_sequence_ = 0;
    std::map<xcb_window_t, WMWindowState> states_;
    xcb_window_t  support_win_ = XCB_NONE;
    xcb_window_t  active_window_ = ~0U;

    // ── DND catcher state ───────────────────────────────────────────────────
    xcb_window_t dnd_catcher_        = 0;
    int      dnd_xfixes_evt_base_ = -1;
    xcb_window_t dnd_source_         = 0;    // yabridge's drag source window
    bool     dnd_requested_      = false; // selection conversion already asked for
    // Payload, fetched while the button is still down. Plain buffer rather than
    // std::string: some callers' headers define min/max as macros, so pulling in
    // <string> alongside them is risky -- keep this layer header-light.
    char     dnd_uri_[8192]      = {};
    bool     dnd_have_uri_       = false;
    bool     dnd_pending_        = false;
};

extern XWaylandWM *g_wm;
