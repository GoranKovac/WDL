#pragma once
#include <map>
#include <vector>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>

struct WMAtoms : xcb_ewmh_connection_t {
    xcb_atom_t WM_DELETE_WINDOW, WM_TAKE_FOCUS, WM_STATE, WM_S0;
    xcb_atom_t _XWAYLAND_ALLOW_COMMITS, _REAPERX_TIMESTAMP;
    xcb_atom_t XdndSelection, XdndAware, XdndEnter, XdndPosition;
    xcb_atom_t XdndStatus, XdndDrop, XdndFinished, XdndLeave, XdndActionCopy;
    xcb_atom_t uri_list, dnd_data, motif_hints;
};

#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL    1
#define WM_STATE_ICONIC    3

struct WMWindowState {
    bool input_hint;
    bool take_focus;
};

class XWaylandWM {
public:
    explicit XWaylandWM(xcb_connection_t *connection);
    xcb_connection_t *connection() const { return connection_; }
    const WMAtoms &atoms() const { return atoms_; }
    bool owns_window(xcb_window_t window, xcb_window_t gui);
    enum WindowType { NORMAL, POPUP, DRAG_ICON };
    WindowType window_type(xcb_window_t window, bool override_redirect);

    static XWaylandWM* init_bridge_wm();
    static const char *display_name();

    void announce_wm(xcb_window_t support_win);

    bool handle_event(xcb_generic_event_t *ev);

    void track_window(xcb_window_t w);
    void untrack_window(xcb_window_t w);

    void set_active_window(xcb_window_t window);
    void send_take_focus(xcb_window_t w, xcb_timestamp_t t);
    void honour_configure_request(xcb_configure_request_event_t *ev);

    void dnd_init();

    bool dnd_has_pending() const { return dnd_pending_; }

    bool dnd_take_pending_path(char *out, size_t outsz);

    void dnd_release_source_button(xcb_connection_t *connection);

    xcb_window_t dnd_catcher_window() const { return dnd_catcher_; }

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

    xcb_window_t dnd_catcher_        = 0;
    int      dnd_xfixes_evt_base_ = -1;
    xcb_window_t dnd_source_         = 0;
    bool     dnd_requested_      = false;
    char     dnd_uri_[8192]      = {};
    bool     dnd_have_uri_       = false;
    bool     dnd_pending_        = false;
};

extern XWaylandWM *g_wm;
