#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

#include "xwayland-bridge-wm.h"
#include "xwayland-bridge-display.h"
#include "swell-xcb.h"
#include <xcb/xtest.h>
#include <xcb/xfixes.h>
#include <xcb/composite.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/prctl.h>
#include <signal.h>
#include <glib.h>
#include <gdk/gdk.h>
#include <algorithm>

XWaylandWM::XWaylandWM(xcb_connection_t *connection) : connection_(connection),
    root_(xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root) {
    init_atoms();
}

static pid_t s_xwayland_pid = 0;
static char s_xwayland_display[sizeof(sockaddr_un::sun_path)];
XWaylandWM *g_wm = nullptr;

struct WMEventSource {
    GSource source;
    GPollFD poll;
    xcb_generic_event_t *event;
};

static gboolean wm_event_prepare(GSource *source, gint *timeout)
{
    *timeout = -1;
    WMEventSource *wm_source = (WMEventSource*)source;
    if (!wm_source->event && g_wm)
        wm_source->event = xcb_poll_for_queued_event(g_wm->connection());
    return wm_source->event != nullptr;
}

static gboolean wm_event_check(GSource *source)
{
    WMEventSource *wm_source = (WMEventSource*)source;
    if (!wm_source->event && g_wm)
        wm_source->event = xcb_poll_for_event(g_wm->connection());
    return wm_source->event || (wm_source->poll.revents & (G_IO_HUP | G_IO_ERR));
}

static gboolean wm_event_dispatch(GSource *source, GSourceFunc, gpointer)
{
    WMEventSource *wm_source = (WMEventSource*)source;
    const gint64 deadline = g_get_monotonic_time() + 2000;
    for (int n = 0; n < 64 && g_wm; ++n) {
        xcb_generic_event_t *ev = wm_source->event;
        wm_source->event = nullptr;
        if (!ev) ev = xcb_poll_for_event(g_wm->connection());
        if (!ev) break;
        g_wm->handle_event(ev);
        if (g_wm->on_unhandled_event) g_wm->on_unhandled_event(ev);
        free(ev);
        if (g_get_monotonic_time() >= deadline) break;
    }
    if (!g_wm || xcb_connection_has_error(g_wm->connection())) return G_SOURCE_REMOVE;
    xcb_flush(g_wm->connection());
    return G_SOURCE_CONTINUE;
}

const char *XWaylandWM::display_name()
{
    return s_xwayland_display;
}

XWaylandWM* XWaylandWM::init_bridge_wm()
{
    if (s_xwayland_pid > 0) return g_wm;

    int wm_fd;
    s_xwayland_pid = xw_spawn_xwayland(s_xwayland_display, sizeof(s_xwayland_display), &wm_fd);
    if (s_xwayland_pid < 0) return nullptr;

    xcb_connection_t *connection = xcb_connect_to_fd(wm_fd, nullptr);
    if (xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        kill(s_xwayland_pid, SIGTERM);
        while (waitpid(s_xwayland_pid, nullptr, 0) < 0 && errno == EINTR) {}
        s_xwayland_pid = 0;
        return nullptr;
    }

    setenv("DISPLAY", s_xwayland_display, 1);
    free(xcb_composite_query_version_reply(connection,
        xcb_composite_query_version(connection, 0, 4), nullptr));
    g_wm = new XWaylandWM(connection);

    xcb_window_t support = xcb_generate_id(connection);
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, support, g_wm->root_,
        -2, -2, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, nullptr);
    xcb_map_window(connection, support);
    g_wm->announce_wm(support);
    g_wm->disallow_commits(support);

    static GSourceFuncs funcs = { wm_event_prepare, wm_event_check, wm_event_dispatch, nullptr, nullptr, nullptr };
    GSource *source = g_source_new(&funcs, sizeof(WMEventSource));
    WMEventSource *wm_source = (WMEventSource*)source;
    wm_source->poll.fd = xcb_get_file_descriptor(connection);
    wm_source->poll.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
    g_source_add_poll(source, &wm_source->poll);
    g_source_set_priority(source, GDK_PRIORITY_REDRAW + 1);
    g_source_attach(source, nullptr);
    g_source_unref(source);

    DEBUG_PRINT("[WM] initialised\n");
    return g_wm;
}

void XWaylandWM::init_atoms() {
    xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(connection_, &atoms_);
    struct { const char *name; xcb_atom_t *atom; } extra[] = {
        { "WM_DELETE_WINDOW", &atoms_.WM_DELETE_WINDOW },
        { "WM_TAKE_FOCUS", &atoms_.WM_TAKE_FOCUS },
        { "WM_STATE", &atoms_.WM_STATE },
        { "WM_S0", &atoms_.WM_S0 },
        { "_XWAYLAND_ALLOW_COMMITS", &atoms_._XWAYLAND_ALLOW_COMMITS },
        { "_REAPERX_TIMESTAMP", &atoms_._REAPERX_TIMESTAMP },
        { "XdndSelection", &atoms_.XdndSelection },
        { "XdndAware", &atoms_.XdndAware },
        { "XdndEnter", &atoms_.XdndEnter },
        { "XdndPosition", &atoms_.XdndPosition },
        { "XdndStatus", &atoms_.XdndStatus },
        { "XdndDrop", &atoms_.XdndDrop },
        { "XdndFinished", &atoms_.XdndFinished },
        { "XdndLeave", &atoms_.XdndLeave },
        { "XdndActionCopy", &atoms_.XdndActionCopy },
        { "text/uri-list", &atoms_.uri_list },
        { "XWB_DND_DATA", &atoms_.dnd_data },
        { "_MOTIF_WM_HINTS", &atoms_.motif_hints }
    };
    xcb_intern_atom_cookie_t requests[sizeof(extra) / sizeof(extra[0])];
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i)
        requests[i] = xcb_intern_atom(connection_, false, strlen(extra[i].name), extra[i].name);
    xcb_ewmh_init_atoms_replies(&atoms_, cookies, nullptr);
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection_, requests[i], nullptr);
        if (reply) *extra[i].atom = reply->atom;
        free(reply);
    }
}

void XWaylandWM::disallow_commits(xcb_window_t w) {
    const uint32_t zero = 0;
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, w, atoms_._XWAYLAND_ALLOW_COMMITS,
                        XCB_ATOM_CARDINAL, 32, 1, &zero);
}

void XWaylandWM::announce_wm(xcb_window_t support_win) {
    support_win_ = support_win;
    set_active_window(XCB_NONE);
    xcb_ewmh_set_supporting_wm_check(&atoms_, root_, support_win);
    xcb_ewmh_set_supporting_wm_check(&atoms_, support_win, support_win);
    const char *name = "REAPERXBridge";
    xcb_ewmh_set_wm_name(&atoms_, support_win, strlen(name), name);
    xcb_atom_t supported[] = {
        atoms_._NET_WM_STATE, atoms_._NET_WM_STATE_MODAL, atoms_._NET_WM_STATE_ABOVE,
        atoms_._NET_ACTIVE_WINDOW, atoms_._NET_WM_WINDOW_TYPE,
        atoms_._NET_WM_WINDOW_TYPE_DIALOG, atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU,
        atoms_._NET_WM_WINDOW_TYPE_NORMAL, atoms_._NET_SUPPORTING_WM_CHECK, atoms_._NET_WM_NAME
    };
    xcb_ewmh_set_supported(&atoms_, 0, sizeof(supported) / sizeof(supported[0]), supported);
    const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                          XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection_, root_, XCB_CW_EVENT_MASK, &mask);
    const uint32_t support_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection_, support_win, XCB_CW_EVENT_MASK, &support_mask);
    xcb_composite_redirect_subwindows(connection_, root_, XCB_COMPOSITE_REDIRECT_MANUAL);
    xcb_set_selection_owner(connection_, support_win, atoms_.WM_S0, XCB_CURRENT_TIME);
    xcb_flush(connection_);
}

void XWaylandWM::set_active_window(xcb_window_t window) {
    if (active_window_ == window) return;
    active_window_ = window;
    xcb_ewmh_set_active_window(&atoms_, 0, window);
    xcb_flush(connection_);
}

void XWaylandWM::track_window(xcb_window_t w) {
    if (states_.count(w)) return;

    WMWindowState st{};
    st.input_hint       = true;

    read_wm_protocols(w, st);
    read_wm_hints(w, st);

    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(connection_, w, XCB_CW_EVENT_MASK, &mask);

    states_[w] = st;
}

void XWaylandWM::untrack_window(xcb_window_t w) {
    states_.erase(w);
    if (pending_focus_ == w) pending_focus_ = XCB_NONE;
}

bool XWaylandWM::handle_event(xcb_generic_event_t *ev) {
    if (dnd_handle_event(ev)) return true;
    switch (ev->response_type & 0x7f) {
    case XCB_CREATE_NOTIFY: return on_create_notify((xcb_create_notify_event_t*)ev);
    case XCB_MAP_REQUEST: return on_map_request((xcb_map_request_event_t*)ev);
    case XCB_CONFIGURE_REQUEST: return on_configure_request((xcb_configure_request_event_t*)ev);
    case XCB_REPARENT_NOTIFY: return on_reparent_notify((xcb_reparent_notify_event_t*)ev);
    case XCB_UNMAP_NOTIFY: return on_unmap_notify((xcb_unmap_notify_event_t*)ev);
    case XCB_DESTROY_NOTIFY: return on_destroy_notify((xcb_destroy_notify_event_t*)ev);
    case XCB_PROPERTY_NOTIFY: {
        xcb_property_notify_event_t *property = (xcb_property_notify_event_t*)ev;
        if (property->window == support_win_ && property->atom == atoms_._REAPERX_TIMESTAMP) {
            if (property->sequence != focus_sequence_) return true;
            auto it = states_.find(pending_focus_);
            if (it != states_.end()) {
                if (it->second.input_hint)
                    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT, pending_focus_, property->time);
                if (it->second.take_focus) send_take_focus(pending_focus_, property->time);
            }
            pending_focus_ = XCB_NONE;
            return true;
        }
        auto it = states_.find(property->window);
        if (it != states_.end()) {
            if (property->atom == atoms_.WM_PROTOCOLS) read_wm_protocols(property->window, it->second);
            if (property->atom == XCB_ATOM_WM_HINTS) read_wm_hints(property->window, it->second);
        }
        break;
    }
    }
    return false;
}

bool XWaylandWM::on_create_notify(xcb_create_notify_event_t *ev) {
    if (ev->parent == root_)
        disallow_commits(ev->window);
    return false;
}

bool XWaylandWM::on_map_request(xcb_map_request_event_t *ev) {
    track_window(ev->window);
    xcb_map_window(connection_, ev->window);
    set_wm_state(ev->window, WM_STATE_NORMAL);
    const WMWindowState &state = states_[ev->window];
    if (state.input_hint || state.take_focus) {
        pending_focus_ = ev->window;
        focus_sequence_ = xcb_change_property(connection_, XCB_PROP_MODE_APPEND, support_win_,
            atoms_._REAPERX_TIMESTAMP, XCB_ATOM_ATOM, 32, 0, nullptr).sequence;
    }
    return true;
}

bool XWaylandWM::on_configure_request(xcb_configure_request_event_t *ev) {
    honour_configure_request(ev);
    return true;
}

void XWaylandWM::honour_configure_request(xcb_configure_request_event_t *ev) {
    xcb_configure_window_value_list_t values = {};
    values.x = ev->x; values.y = ev->y;
    values.width = ev->width; values.height = ev->height;
    values.border_width = ev->border_width;
    values.sibling = ev->sibling; values.stack_mode = ev->stack_mode;
    uint16_t mask = ev->value_mask;
    if (!(mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y))) {
        xcb_size_hints_t hints = {};
        if (xcb_icccm_get_wm_normal_hints_reply(connection_,
            xcb_icccm_get_wm_normal_hints(connection_, ev->window), &hints, nullptr) &&
            (hints.flags & (XCB_ICCCM_SIZE_HINT_P_POSITION | XCB_ICCCM_SIZE_HINT_US_POSITION))) {
            values.x = hints.x; values.y = hints.y;
            mask |= XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
        }
    }
    if (mask) xcb_configure_window_aux(connection_, ev->window, mask, &values);
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection_,
        xcb_get_geometry(connection_, ev->window), nullptr);
    if (geometry) {
        xcb_configure_notify_event_t event = {};
        event.response_type = XCB_CONFIGURE_NOTIFY;
        event.event = event.window = ev->window;
        event.x = geometry->x; event.y = geometry->y;
        event.width = geometry->width; event.height = geometry->height;
        event.border_width = geometry->border_width;
        xcb_send_event(connection_, false, ev->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char*)&event);
        free(geometry);
    }
    xcb_flush(connection_);
}

bool XWaylandWM::on_reparent_notify(xcb_reparent_notify_event_t *ev) {
    if (ev->parent == root_) {
        disallow_commits(ev->window);
        return false;
    }

    return false;
}

bool XWaylandWM::on_unmap_notify(xcb_unmap_notify_event_t *ev) {
    auto it = states_.find(ev->window);
    if (it == states_.end()) return false;

    set_wm_state(ev->window, WM_STATE_WITHDRAWN);
    if (pending_focus_ == ev->window) pending_focus_ = XCB_NONE;

    return false;
}

bool XWaylandWM::on_destroy_notify(xcb_destroy_notify_event_t *ev) {
    untrack_window(ev->window);
    return false;
}

void XWaylandWM::send_take_focus(xcb_window_t w, xcb_timestamp_t t) {
    xcb_client_message_event_t event = {};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = w;
    event.type = atoms_.WM_PROTOCOLS;
    event.format = 32;
    event.data.data32[0] = atoms_.WM_TAKE_FOCUS;
    event.data.data32[1] = t;
    xcb_send_event(connection_, false, w, XCB_EVENT_MASK_NO_EVENT, (const char*)&event);
}

void XWaylandWM::set_wm_state(xcb_window_t w, int state) {
    const uint32_t data[] = { (uint32_t)state, XCB_NONE };
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, w, atoms_.WM_STATE, atoms_.WM_STATE, 32, 2, data);
}

void XWaylandWM::read_wm_protocols(xcb_window_t w, WMWindowState &st) {
    st.take_focus = false;
    xcb_icccm_get_wm_protocols_reply_t protocols;
    if (!xcb_icccm_get_wm_protocols_reply(connection_,
        xcb_icccm_get_wm_protocols(connection_, w, atoms_.WM_PROTOCOLS), &protocols, nullptr)) return;
    for (uint32_t i = 0; i < protocols.atoms_len; ++i)
        if (protocols.atoms[i] == atoms_.WM_TAKE_FOCUS) st.take_focus = true;
    xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
}

void XWaylandWM::read_wm_hints(xcb_window_t w, WMWindowState &st) {
    xcb_icccm_wm_hints_t hints;
    st.input_hint = true;
    if (xcb_icccm_get_wm_hints_reply(connection_, xcb_icccm_get_wm_hints(connection_, w), &hints, nullptr) &&
        (hints.flags & XCB_ICCCM_WM_HINT_INPUT)) st.input_hint = hints.input;
}

bool XWaylandWM::owns_window(xcb_window_t window, xcb_window_t gui)
{
    uint32_t window_pid = 0, gui_pid = 0;
    xcb_get_property_cookie_t a = xcb_ewmh_get_wm_pid(&atoms_, window);
    xcb_get_property_cookie_t b = xcb_ewmh_get_wm_pid(&atoms_, gui);
    xcb_ewmh_get_wm_pid_reply(&atoms_, a, &window_pid, nullptr);
    xcb_ewmh_get_wm_pid_reply(&atoms_, b, &gui_pid, nullptr);
    return window_pid && window_pid == gui_pid;
}

XWaylandWM::WindowType XWaylandWM::window_type(xcb_window_t window, bool override_redirect)
{
    xcb_ewmh_get_atoms_reply_t types;
    xcb_atom_t type = XCB_NONE;
    if (xcb_ewmh_get_wm_window_type_reply(&atoms_, xcb_ewmh_get_wm_window_type(&atoms_, window), &types, nullptr)) {
        for (uint32_t i = 0; i < types.atoms_len; ++i) {
            const xcb_atom_t candidate = types.atoms[i];
            if (candidate == atoms_._NET_WM_WINDOW_TYPE_DND) { type = candidate; break; }
            if (!type && (candidate == atoms_._NET_WM_WINDOW_TYPE_NORMAL ||
                candidate == atoms_._NET_WM_WINDOW_TYPE_DIALOG || candidate == atoms_._NET_WM_WINDOW_TYPE_UTILITY ||
                candidate == atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU || candidate == atoms_._NET_WM_WINDOW_TYPE_MENU ||
                candidate == atoms_._NET_WM_WINDOW_TYPE_DROPDOWN_MENU || candidate == atoms_._NET_WM_WINDOW_TYPE_TOOLTIP))
                type = candidate;
        }
        xcb_ewmh_get_atoms_reply_wipe(&types);
    }
    if (type == atoms_._NET_WM_WINDOW_TYPE_DND) return DRAG_ICON;
    if (override_redirect) return POPUP;
    if (type == atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU || type == atoms_._NET_WM_WINDOW_TYPE_MENU ||
        type == atoms_._NET_WM_WINDOW_TYPE_DROPDOWN_MENU || type == atoms_._NET_WM_WINDOW_TYPE_TOOLTIP) return POPUP;
    if (!type) {
        xcb_window_t transient = XCB_NONE;
        xcb_icccm_get_wm_transient_for_reply(connection_,
            xcb_icccm_get_wm_transient_for(connection_, window), &transient, nullptr);
        type = transient ? atoms_._NET_WM_WINDOW_TYPE_DIALOG : atoms_._NET_WM_WINDOW_TYPE_NORMAL;
    }
    if (type != atoms_._NET_WM_WINDOW_TYPE_NORMAL) return NORMAL;

    xcb_get_property_reply_t *motif = xcb_get_property_reply(connection_,
        xcb_get_property(connection_, false, window, atoms_.motif_hints, atoms_.motif_hints, 0, 5), nullptr);
    bool undecorated = false;
    if (motif && motif->format == 32 && motif->value_len >= 3) {
        const uint32_t *values = (const uint32_t*)xcb_get_property_value(motif);
        undecorated = (values[0] & (1U << 1)) && !values[2];
    }
    free(motif);
    return undecorated ? POPUP : NORMAL;
}

void XWaylandWM::dnd_init()
{
    if (dnd_catcher_) return;
    const xcb_query_extension_reply_t *extension = xcb_get_extension_data(connection_, &xcb_xfixes_id);
    if (!extension || !extension->present) return;
    xcb_xfixes_query_version_reply_t *version = xcb_xfixes_query_version_reply(connection_,
        xcb_xfixes_query_version(connection_, 5, 0), nullptr);
    if (!version) return;
    free(version);
    dnd_xfixes_evt_base_ = extension->first_event;
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection_)).data;
    dnd_catcher_ = xcb_generate_id(connection_);
    const uint32_t override_redirect = true;
    xcb_create_window(connection_, XCB_COPY_FROM_PARENT, dnd_catcher_, root_, 0, 0,
        screen->width_in_pixels, screen->height_in_pixels, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
    const uint32_t ver = 5;
    xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, dnd_catcher_, atoms_.XdndAware,
                        XCB_ATOM_ATOM, 32, 1, &ver);
    xcb_xfixes_select_selection_input(connection_, root_, atoms_.XdndSelection,
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER);
    xcb_flush(connection_);
}

void XWaylandWM::dnd_catcher_show(bool on)
{
    if (!dnd_catcher_) return;
    if (on) {
        const uint32_t above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(connection_, dnd_catcher_, XCB_CONFIG_WINDOW_STACK_MODE, &above);
        xcb_map_window(connection_, dnd_catcher_);
    } else {
        xcb_unmap_window(connection_, dnd_catcher_);
        dnd_source_ = XCB_NONE;
    }
    xcb_flush(connection_);
}

void XWaylandWM::dnd_send_status(xcb_window_t src, bool accepted)
{
    xcb_client_message_event_t event = {};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = src;
    event.type = atoms_.XdndStatus;
    event.format = 32;
    event.data.data32[0] = dnd_catcher_;
    event.data.data32[1] = accepted ? 1 : 0;
    event.data.data32[4] = accepted ? atoms_.XdndActionCopy : XCB_NONE;
    xcb_send_event(connection_, false, src, XCB_EVENT_MASK_NO_EVENT, (const char*)&event);
    xcb_flush(connection_);
}

void XWaylandWM::dnd_send_finished(xcb_window_t src, bool accepted)
{
    xcb_client_message_event_t event = {};
    event.response_type = XCB_CLIENT_MESSAGE;
    event.window = src;
    event.type = atoms_.XdndFinished;
    event.format = 32;
    event.data.data32[0] = dnd_catcher_;
    event.data.data32[1] = accepted ? 1 : 0;
    event.data.data32[2] = accepted ? atoms_.XdndActionCopy : XCB_NONE;
    xcb_send_event(connection_, false, src, XCB_EVENT_MASK_NO_EVENT, (const char*)&event);
    xcb_flush(connection_);
}

void XWaylandWM::dnd_reset_payload()
{
    dnd_requested_ = false;
    dnd_have_uri_  = false;
    dnd_pending_   = false;
    dnd_uri_[0]    = 0;
}

bool XWaylandWM::dnd_handle_event(xcb_generic_event_t *ev)
{
    if (!dnd_catcher_ || !connection_) return false;

    if (dnd_xfixes_evt_base_ >= 0 &&
        (ev->response_type & 0x7f) == dnd_xfixes_evt_base_ + XCB_XFIXES_SELECTION_NOTIFY)
    {
        xcb_xfixes_selection_notify_event_t *se = (xcb_xfixes_selection_notify_event_t*)ev;
        if (se->selection == atoms_.XdndSelection) {
            if (se->owner != XCB_NONE) dnd_reset_payload();
            dnd_catcher_show(se->owner != XCB_NONE);
        }
        return true;
    }

    if ((ev->response_type & 0x7f) == XCB_CLIENT_MESSAGE && ((xcb_client_message_event_t*)ev)->window == dnd_catcher_)
    {
        const xcb_atom_t   mt  = ((xcb_client_message_event_t*)ev)->type;
        const xcb_window_t src = (xcb_window_t)((xcb_client_message_event_t*)ev)->data.data32[0];

        if (mt == atoms_.XdndEnter) {
            dnd_source_ = src;
            return true;
        }
        if (mt == atoms_.XdndPosition) {
            dnd_send_status(src, true);

            if (!dnd_requested_) {
                dnd_requested_ = true;
                dnd_source_    = src;
                xcb_convert_selection(connection_, dnd_catcher_, atoms_.XdndSelection, atoms_.uri_list,
                                      atoms_.dnd_data, XCB_CURRENT_TIME);
                xcb_flush(connection_);
            }
            return true;
        }
        if (mt == atoms_.XdndLeave) {
            dnd_catcher_show(false);
            dnd_reset_payload();
            return true;
        }
        if (mt == atoms_.XdndDrop) {
            if (!dnd_have_uri_) {
                const xcb_timestamp_t when = (xcb_timestamp_t)((xcb_client_message_event_t*)ev)->data.data32[2];
                xcb_convert_selection(connection_, dnd_catcher_, atoms_.XdndSelection, atoms_.uri_list,
                                      atoms_.dnd_data, when);
                xcb_flush(connection_);
                dnd_source_ = src;
                return true;
            }
            dnd_send_finished(src, true);
            dnd_catcher_show(false);
            dnd_reset_payload();
            return true;
        }
        return true;
    }

    if ((ev->response_type & 0x7f) == XCB_SELECTION_NOTIFY && ((xcb_selection_notify_event_t*)ev)->requestor == dnd_catcher_)
    {
        if (((xcb_selection_notify_event_t*)ev)->property != XCB_NONE) {
            xcb_get_property_reply_t *reply = xcb_get_property_reply(connection_,
                xcb_get_property(connection_, true, dnd_catcher_, atoms_.dnd_data,
                                 atoms_.uri_list, 0, sizeof(dnd_uri_) / 4), nullptr);
            if (reply && reply->format == 8 && !reply->bytes_after) {
                size_t len = xcb_get_property_value_length(reply);
                if (len < sizeof(dnd_uri_)) {
                    memcpy(dnd_uri_, xcb_get_property_value(reply), len);
                    dnd_uri_[len] = 0;
                    dnd_have_uri_ = dnd_pending_ = true;
                }
            }
            free(reply);
        }
        return true;
    }

    return false;
}

bool XWaylandWM::dnd_take_pending_path(char *out, size_t outsz)
{
    if (out && outsz) out[0] = 0;
    if (!dnd_pending_) return false;
    dnd_pending_ = false;
    if (!out || !outsz) return false;
    gchar **uris = g_uri_list_extract_uris(dnd_uri_);
    gchar *path = uris && uris[0] ? g_filename_from_uri(uris[0], nullptr, nullptr) : nullptr;
    if (path && strlen(path) < outsz) g_strlcpy(out, path, outsz);
    g_free(path);
    g_strfreev(uris);
    return out[0] != 0;
}

void XWaylandWM::dnd_release_source_button(xcb_connection_t *connection)
{
    if (!connection) return;
    xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(connection,
        xcb_query_pointer(connection, root_), nullptr);
    if (pointer) {
        xcb_test_fake_input(connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME,
                            root_, pointer->root_x, pointer->root_y, 0);
        free(pointer);
    }
    xcb_test_fake_input(connection, XCB_BUTTON_RELEASE, 1, XCB_CURRENT_TIME, root_, 0, 0, 0);
    free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr));
}
