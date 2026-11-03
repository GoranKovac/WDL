#include <gtk/gtk.h>
#include <gdk/gdkwayland.h>
#include <cairo-xcb.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <drm_fourcc.h>
#include <wayland-egl.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/dri3.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/sync.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>
#include <xcb/xtest.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#include "swell-xwayland-bridge.h"
#include "swell-wayland.h"
#include "swell-internal.h"
#include "swell-gdk-scale.h"
#include "swell-xcb.h"

#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

struct WaylandConnection {
    wl_display *display;
    int socket;
    std::mutex mutex;
    std::set<uint32_t> registries;

    WaylandConnection(wl_display *d, int fd) : display(d), socket(fd) {}
    ~WaylandConnection() { close(socket); wl_display_disconnect(display); }

    void stop()
    {
        shutdown(socket, SHUT_RDWR);
        shutdown(wl_display_get_fd(display), SHUT_RDWR);
    }
};

struct WaylandMessage {
    uint32_t header[2];
    std::vector<uint32_t> body;
    std::vector<int> fds;

    ~WaylandMessage() { close_fds(); }

    void close_fds()
    {
        for (int fd : fds) close(fd);
        fds.clear();
    }

    bool read_bytes(int socket, void *data, size_t size)
    {
        while (size) {
            union { cmsghdr align; char data[CMSG_SPACE(28 * sizeof(int))]; } control;
            iovec iov = { data, size };
            msghdr msg = {};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            msg.msg_control = control.data;
            msg.msg_controllen = sizeof(control.data);
            ssize_t received = recvmsg(socket, &msg, MSG_CMSG_CLOEXEC);
            if (received < 0 && errno == EINTR) continue;
            if (received <= 0) return false;
            for (cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
                const int *received_fds = (const int*)CMSG_DATA(cmsg);
                const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                fds.insert(fds.end(), received_fds, received_fds + count);
            }
            if (msg.msg_flags & MSG_CTRUNC) return false;
            data = (char*)data + received;
            size -= received;
        }
        return true;
    }

    bool read(int socket)
    {
        if (!read_bytes(socket, header, sizeof(header))) return false;
        const size_t size = header[1] >> 16;
        if (size < sizeof(header) || size % 4) return false;
        body.resize((size - sizeof(header)) / sizeof(uint32_t));
        return read_bytes(socket, body.data(), size - sizeof(header));
    }

    bool write_bytes(int socket, const void *data, size_t size)
    {
        while (size) {
            iovec iov = { (void*)data, size };
            msghdr msg = {};
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            std::vector<char> control;
            if (!fds.empty()) {
                control.resize(CMSG_SPACE(fds.size() * sizeof(int)));
                msg.msg_control = control.data();
                msg.msg_controllen = control.size();
                cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                cmsg->cmsg_level = SOL_SOCKET;
                cmsg->cmsg_type = SCM_RIGHTS;
                cmsg->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
                memcpy(CMSG_DATA(cmsg), fds.data(), fds.size() * sizeof(int));
            }
            ssize_t sent = sendmsg(socket, &msg, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) return false;
            close_fds();
            data = (const char*)data + sent;
            size -= sent;
        }
        return true;
    }

    bool write(int socket)
    {
        return write_bytes(socket, header, sizeof(header)) &&
            write_bytes(socket, body.data(), body.size() * sizeof(uint32_t));
    }
};

static void relay_wayland(std::shared_ptr<WaylandConnection> connection, bool from_server)
{
    const int server = wl_display_get_fd(connection->display);
    const int source = from_server ? server : connection->socket;
    const int destination = from_server ? connection->socket : server;
    WaylandMessage message;
    std::set<std::pair<uint32_t, uint32_t>> hidden_globals;
    while (message.read(source)) {
        const uint32_t object = message.header[0], opcode = message.header[1] & 0xffff;
        bool is_registry;
        {
            std::lock_guard<std::mutex> lock(connection->mutex);
            if (!from_server && object == 1 && opcode == WL_DISPLAY_GET_REGISTRY && message.body.size() == 1)
                connection->registries.insert(message.body[0]);
            is_registry = connection->registries.count(object) != 0;
        }
        if (from_server && is_registry) {
            if (opcode == 0 && message.body.size() >= 2) {
                static const char *const hidden[] = { "zxdg_output_manager_v1", "xwayland_shell_v1" };
                bool hide = false;
                for (const char *interface : hidden) {
                    const size_t len = strlen(interface) + 1;
                    if (message.body[1] == len &&
                        (message.body.size() - 2) * sizeof(uint32_t) >= len &&
                        !memcmp(&message.body[2], interface, len)) { hide = true; break; }
                }
                if (hide) {
                    hidden_globals.insert({object, message.body[0]});
                    continue;
                }
            } else if (opcode == 1 && message.body.size() == 1 &&
                       hidden_globals.erase({object, message.body[0]})) continue;
        }
        if (!message.write(destination)) break;
    }
    connection->stop();
}

static char *socket_directory;
static sockaddr_un socket_address;

static void remove_xwayland_socket()
{
    if (!socket_directory) return;
    unlink(socket_address.sun_path);
    rmdir(socket_directory);
    g_free(socket_directory);
    socket_directory = nullptr;
}

static int create_xwayland_socket(char *display_name, size_t size)
{
    remove_xwayland_socket();
    socket_directory = g_build_filename(g_get_user_runtime_dir(), "swell-xwayland-XXXXXX", nullptr);
    if (!g_mkdtemp(socket_directory)) {
        g_free(socket_directory);
        socket_directory = nullptr;
        return -1;
    }
    socket_address.sun_family = AF_UNIX;
    const int length = snprintf(socket_address.sun_path, sizeof(socket_address.sun_path), "%s/X", socket_directory);
    if (length < 0 || (size_t)length >= sizeof(socket_address.sun_path) || (size_t)length >= size) {
        socket_address.sun_path[0] = 0;
        remove_xwayland_socket();
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (sockaddr*)&socket_address, sizeof(socket_address)) < 0 || listen(fd, SOMAXCONN) < 0) {
        if (fd >= 0) close(fd);
        remove_xwayland_socket();
        return -1;
    }
    memcpy(display_name, socket_address.sun_path, length + 1);
    return fd;
}

static pid_t xw_spawn_xwayland(char *display_name, size_t size, int *wm_fd)
{
    *wm_fd = -1;
    wl_display *display = wl_display_connect(nullptr);
    if (!display) return -1;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
        wl_display_disconnect(display);
        return -1;
    }
    std::shared_ptr<WaylandConnection> connection(new WaylandConnection(display, sockets[0]));
    const int server = wl_display_get_fd(display);
    const int flags = fcntl(server, F_GETFL);
    if (flags < 0 || fcntl(server, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        close(sockets[1]);
        return -1;
    }
    int ready[2] = { -1, -1 }, wm[2] = { -1, -1 };
    const int listener = create_xwayland_socket(display_name, size);
    if (listener < 0 || pipe2(ready, O_CLOEXEC) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) < 0) {
        for (int fd : { listener, ready[0], ready[1], wm[0], wm[1], sockets[1] })
            if (fd >= 0) close(fd);
        remove_xwayland_socket();
        return -1;
    }
    char ready_fd[32], wm_fd_string[32], listener_fd[32], socket_env[64];
    snprintf(ready_fd, sizeof(ready_fd), "%d", ready[1]);
    snprintf(wm_fd_string, sizeof(wm_fd_string), "%d", wm[1]);
    snprintf(listener_fd, sizeof(listener_fd), "%d", listener);
    snprintf(socket_env, sizeof(socket_env), "WAYLAND_SOCKET=%d", sockets[1]);
    extern char **environ;
    std::vector<char*> env;
    for (char **p = environ; *p; ++p)
        if (strncmp(*p, "WAYLAND_SOCKET=", 15) && strncmp(*p, "DISPLAY=", 8)) env.push_back(*p);
    env.push_back(socket_env);
    env.push_back(nullptr);
    char *args[] = { (char*)"Xwayland", (char*)"-displayfd", ready_fd,
                     (char*)"-wm", wm_fd_string, (char*)"-listenfd", listener_fd,
                     (char*)"-rootless", (char*)"-nolisten", (char*)"tcp", nullptr };
    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) _exit(1);
        close(ready[0]);
        close(wm[0]);
        close(sockets[0]);
        close(server);
        for (int fd : { ready[1], wm[1], listener, sockets[1] })
            if (fcntl(fd, F_SETFD, 0) < 0) _exit(1);
        execve("/usr/bin/Xwayland", args, env.data());
        _exit(1);
    }
    close(ready[1]);
    close(wm[1]);
    close(listener);
    close(sockets[1]);
    if (pid > 0) {
        std::thread to_server, to_client;
        try {
            to_server = std::thread(relay_wayland, connection, false);
            to_client = std::thread(relay_wayland, connection, true);
        } catch (...) {
            close(ready[0]);
            close(wm[0]);
            connection->stop();
            if (to_server.joinable()) to_server.join();
            if (to_client.joinable()) to_client.join();
            kill(pid, SIGTERM);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            remove_xwayland_socket();
            return -1;
        }
        to_server.detach();
        to_client.detach();
    }
    char display_number[32];
    size_t length = 0;
    const gint64 deadline = g_get_monotonic_time() + 10000000;
    while (pid > 0 && length < sizeof(display_number)) {
        const int remaining = (deadline - g_get_monotonic_time()) / 1000;
        if (remaining <= 0) break;
        pollfd fd = { ready[0], POLLIN, 0 };
        const int result = poll(&fd, 1, remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        const ssize_t count = read(ready[0], display_number + length, sizeof(display_number) - length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        length += count;
        if (display_number[length - 1] == '\n') {
            display_number[--length] = 0;
            if (!length || strspn(display_number, "0123456789") != length) break;
            close(ready[0]);
            *wm_fd = wm[0];
            atexit(remove_xwayland_socket);
            return pid;
        }
    }
    close(ready[0]);
    close(wm[0]);
    if (pid > 0) {
        kill(pid, SIGTERM);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
    remove_xwayland_socket();
    return -1;
}

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

XWaylandWM::XWaylandWM(xcb_connection_t *connection) : connection_(connection),
    root_(xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root) {
    init_atoms();
}

static pid_t s_xwayland_pid = 0;
static char s_xwayland_display[sizeof(sockaddr_un::sun_path)];
static XWaylandWM *g_wm = nullptr;

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
        XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
        XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
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
            dnd_reset_payload();
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

class CapturePixmap {
public:
    xcb_connection_t *connection = nullptr;
    xcb_screen_t *screen = nullptr;
    xcb_pixmap_t pixmap = 0;
    int width = 0, height = 0;
    xcb_render_pictforminfo_t format = {};
    cairo_format_t image_format = CAIRO_FORMAT_INVALID;

    CapturePixmap() = default;
    CapturePixmap(const CapturePixmap&) = delete;
    CapturePixmap& operator=(const CapturePixmap&) = delete;
    ~CapturePixmap() { reset(); }
    void reset();
    bool update(xcb_connection_t *conn, xcb_window_t window);
    cairo_surface_t *surface();
};

class CaptureImage {
    xcb_connection_t *connection = nullptr;
    xcb_image_t *image = nullptr;
    xcb_shm_segment_info_t shm = {};
    cairo_surface_t *surface = nullptr;
    bool attached = false, disabled = false;

    bool create(const CapturePixmap &pixmap);

public:
    ~CaptureImage() { reset(); }
    void reset();
    cairo_surface_t *read(const CapturePixmap &pixmap, bool refresh);
};

void CapturePixmap::reset()
{
    if (pixmap) xcb_free_pixmap(connection, pixmap);
    pixmap = 0;
    width = height = 0;
    connection = nullptr;
    screen = nullptr;
    format = {};
    image_format = CAIRO_FORMAT_INVALID;
}

bool CapturePixmap::update(xcb_connection_t *conn, xcb_window_t window)
{
    if (!conn || xcb_connection_has_error(conn)) return false;
    if (connection != conn) reset();
    connection = conn;
    const xcb_pixmap_t next = xcb_generate_id(conn);
    xcb_composite_name_window_pixmap(conn, window, next);
    xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry(conn, next);
    xcb_get_window_attributes_cookie_t attributes_cookie = {};
    if (!format.id) attributes_cookie = xcb_get_window_attributes(conn, window);
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(conn, geometry_cookie, nullptr);
    if (!format.id) {
        xcb_get_window_attributes_reply_t *attributes =
            xcb_get_window_attributes_reply(conn, attributes_cookie, nullptr);
        const xcb_render_query_pict_formats_reply_t *formats = xcb_render_util_query_formats(conn);
        xcb_render_pictvisual_t *visual = attributes && formats ?
            xcb_render_util_find_visual_format(formats, attributes->visual) : nullptr;
        if (visual) {
            xcb_render_pictforminfo_t match = {};
            match.id = visual->format;
            xcb_render_pictforminfo_t *info = xcb_render_util_find_format(formats, XCB_PICT_FORMAT_ID, &match, 0);
            if (info) format = *info;
        }
        free(attributes);
        if (format.type == XCB_RENDER_PICT_TYPE_DIRECT &&
            (format.depth == 24 || format.depth == 32) &&
            format.direct.red_shift == 16 && format.direct.red_mask == 0xff &&
            format.direct.green_shift == 8 && format.direct.green_mask == 0xff &&
            format.direct.blue_shift == 0 && format.direct.blue_mask == 0xff &&
            (!format.direct.alpha_mask ||
             (format.direct.alpha_shift == 24 && format.direct.alpha_mask == 0xff)))
            image_format = format.direct.alpha_mask ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
    }
    if (!geometry || !format.id || geometry->depth != format.depth) {
        free(geometry);
        xcb_free_pixmap(conn, next);
        return false;
    }
    for (xcb_screen_iterator_t i = xcb_setup_roots_iterator(xcb_get_setup(conn)); i.rem; xcb_screen_next(&i))
        if (i.data->root == geometry->root) { screen = i.data; break; }
    if (pixmap) xcb_free_pixmap(conn, pixmap);
    pixmap = next;
    width = geometry->width;
    height = geometry->height;
    free(geometry);
    return screen != nullptr;
}

cairo_surface_t *CapturePixmap::surface()
{
    return cairo_xcb_surface_create_with_xrender_format(connection, screen, pixmap, &format, width, height);
}

bool CaptureImage::create(const CapturePixmap &pixmap)
{
    connection = pixmap.connection;
    if (pixmap.image_format == CAIRO_FORMAT_INVALID || pixmap.width <= 0 || pixmap.height <= 0) return false;
    xcb_shm_query_version_reply_t *version = xcb_shm_query_version_reply(connection,
        xcb_shm_query_version(connection), nullptr);
    const bool supported = version &&
        (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2));
    free(version);
    if (!supported) return false;
    image = xcb_image_create_native(connection, pixmap.width, pixmap.height,
        XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap.format.depth, nullptr, ~0U, nullptr);
    const unsigned int endian = 1;
    if (!image || image->bpp != 32 || !image->stride || image->stride > INT_MAX ||
        (size_t)image->height > SIZE_MAX / image->stride ||
        image->size != (size_t)image->height * image->stride ||
        image->byte_order != (*(const char*)&endian ? XCB_IMAGE_ORDER_LSB_FIRST : XCB_IMAGE_ORDER_MSB_FIRST))
        return false;
    int fd = memfd_create("swell-plugin", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return false;
    if (ftruncate(fd, image->size) < 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
        close(fd);
        return false;
    }
    void *mapping = mmap(nullptr, image->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) { close(fd); return false; }
    image->data = (uint8_t*)mapping;
    shm.shmaddr = image->data;
    shm.shmseg = xcb_generate_id(connection);
    xcb_generic_error_t *error = xcb_request_check(connection,
        xcb_shm_attach_fd_checked(connection, shm.shmseg, fd, false));
    attached = !error && !xcb_connection_has_error(connection);
    free(error);
    if (!attached) return false;
    surface = cairo_image_surface_create_for_data(image->data, pixmap.image_format,
        image->width, image->height, image->stride);
    return cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
}

void CaptureImage::reset()
{
    if (surface) cairo_surface_destroy(surface);
    surface = nullptr;
    if (attached) xcb_shm_detach(connection, shm.shmseg);
    attached = false;
    if (image) {
        if (image->data) munmap(image->data, image->size);
        xcb_image_destroy(image);
    }
    image = nullptr;
    shm = {};
    connection = nullptr;
}

cairo_surface_t *CaptureImage::read(const CapturePixmap &pixmap, bool refresh)
{
    if (disabled) return nullptr;
    if (image && (connection != pixmap.connection || image->width != pixmap.width || image->height != pixmap.height))
        reset();
    if (image && !refresh) return cairo_surface_reference(surface);
    if (!image && !create(pixmap)) {
        reset();
        disabled = true;
        return nullptr;
    }
    cairo_surface_flush(surface);
    if (!xcb_image_shm_get(connection, pixmap.pixmap, image, shm, 0, 0, ~0U)) {
        reset();
        disabled = true;
        return nullptr;
    }
    cairo_surface_mark_dirty(surface);
    return cairo_surface_reference(surface);
}

class CaptureGL {
    GdkGLContext *context = nullptr;
    GdkWindow *context_window = nullptr;
    xcb_connection_t *connection = nullptr;
    unsigned int fence = 0;
    struct wl_egl_window *native_window = nullptr;
    struct wl_surface *surface = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface target = EGL_NO_SURFACE;
    unsigned int texture = 0, framebuffer = 0;
    int width = 0, height = 0;
    bool disabled = false;

    bool make_current();
    bool read(const CapturePixmap &pixmap, GdkWindow *window);

public:
    ~CaptureGL() { reset(); }
    void reset();
    bool enabled() const { return !disabled; }
    bool draw(GtkWidget *widget, const CapturePixmap &pixmap);
};

struct CaptureDmaBuf {
    xcb_dri3_buffers_from_pixmap_reply_t *reply = nullptr;
    int *fds = nullptr;

    ~CaptureDmaBuf()
    {
        if (fds) for (int i = 0; i < reply->nfd; ++i) close(fds[i]);
        free(reply);
    }

    bool get(xcb_connection_t *connection, xcb_pixmap_t pixmap, xcb_sync_fence_t &fence)
    {
        if (!connection || xcb_connection_has_error(connection)) return false;
        xcb_generic_error_t *error = nullptr;
        if (!fence) {
            const xcb_query_extension_reply_t *extension = xcb_get_extension_data(connection, &xcb_dri3_id);
            if (!extension || !extension->present) return false;
            xcb_dri3_query_version_reply_t *version = xcb_dri3_query_version_reply(connection,
                xcb_dri3_query_version(connection, 1, 2), &error);
            bool supported = !error && version &&
                (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2));
            free(version);
            free(error);
            if (!supported) return false;
            xcb_sync_fence_t id = xcb_generate_id(connection);
            error = xcb_request_check(connection, xcb_sync_create_fence_checked(connection, pixmap, id, true));
            supported = !error && !xcb_connection_has_error(connection);
            free(error);
            if (!supported) return false;
            fence = id;
        }
        xcb_sync_reset_fence(connection, fence);
        xcb_sync_trigger_fence(connection, fence);
        error = nullptr;
        reply = xcb_dri3_buffers_from_pixmap_reply(connection,
            xcb_dri3_buffers_from_pixmap(connection, pixmap), &error);
        if (reply) fds = xcb_dri3_buffers_from_pixmap_reply_fds(connection, reply);
        bool supported = !error && reply && reply->nfd > 0 && reply->nfd <= 4;
        free(error);
        return supported;
    }

    EGLImageKHR import(EGLDisplay display)
    {
        if (!epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import")) return EGL_NO_IMAGE_KHR;
        const bool modifiers = epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import_modifiers");
        if (!modifiers && reply->modifier != DRM_FORMAT_MOD_INVALID &&
            reply->modifier != DRM_FORMAT_MOD_LINEAR) return EGL_NO_IMAGE_KHR;
        const uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(reply);
        const uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(reply);
        static const EGLint planes[][5] = {
            { EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
              EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
              EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
              EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
            { EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
              EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT }
        };
        std::vector<EGLint> attrs = {
            EGL_WIDTH, reply->width, EGL_HEIGHT, reply->height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)(reply->depth == 32 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888)
        };
        for (int i = 0; i < reply->nfd; ++i) {
            if (strides[i] > INT_MAX || offsets[i] > INT_MAX) return EGL_NO_IMAGE_KHR;
            attrs.insert(attrs.end(), { planes[i][0], fds[i], planes[i][1], (EGLint)offsets[i],
                                        planes[i][2], (EGLint)strides[i] });
            if (modifiers && reply->modifier != DRM_FORMAT_MOD_INVALID)
                attrs.insert(attrs.end(), { planes[i][3], (EGLint)reply->modifier,
                                            planes[i][4], (EGLint)(reply->modifier >> 32) });
        }
        attrs.push_back(EGL_NONE);
        return eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    }
};

bool CaptureGL::make_current()
{
    gdk_gl_context_make_current(context);
    if (gdk_gl_context_get_current() != context || eglGetCurrentDisplay() == EGL_NO_DISPLAY) return false;
    return target == EGL_NO_SURFACE || eglMakeCurrent(display, target, target, eglGetCurrentContext());
}

void CaptureGL::reset()
{
    if (fence) {
        xcb_sync_destroy_fence(connection, fence);
        xcb_flush(connection);
        fence = 0;
    }
    connection = nullptr;
    if (context) {
        GdkGLContext *previous = gdk_gl_context_get_current();
        if (previous) g_object_ref(previous);
        if (make_current()) {
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteTextures(1, &texture);
        }
        if (previous && previous != context) gdk_gl_context_make_current(previous);
        else gdk_gl_context_clear_current();
        if (previous) g_object_unref(previous);
        g_object_unref(context);
        context = nullptr;
    }
    if (target != EGL_NO_SURFACE) eglDestroySurface(display, target);
    if (native_window) wl_egl_window_destroy(native_window);
    if (context_window) gdk_window_destroy(context_window);
    context_window = nullptr;
    target = EGL_NO_SURFACE;
    display = EGL_NO_DISPLAY;
    native_window = nullptr;
    surface = nullptr;
    texture = framebuffer = 0;
    width = height = 0;
}

bool CaptureGL::read(const CapturePixmap &pixmap, GdkWindow *window)
{
    if (pixmap.image_format == CAIRO_FORMAT_INVALID) return false;
    const int w = pixmap.width, h = pixmap.height;
    CaptureDmaBuf buffer;
    connection = pixmap.connection;
    if (!buffer.get(connection, pixmap.pixmap, fence) || buffer.reply->width != w || buffer.reply->height != h ||
        buffer.reply->bpp != 32 || buffer.reply->depth != pixmap.format.depth) return false;

    if (!context) {
        GdkWindowAttr attr = {};
        attr.window_type = GDK_WINDOW_TEMP;
        attr.wclass = GDK_INPUT_OUTPUT;
        attr.width = attr.height = 1;
        attr.visual = gdk_window_get_visual(window);
        context_window = gdk_window_new(gdk_screen_get_root_window(gdk_window_get_screen(window)),
                                        &attr, GDK_WA_VISUAL);
        GError *error = nullptr;
        context = gdk_window_create_gl_context(context_window, &error);
        if (error) g_error_free(error);
        if (!context) return false;
        gdk_gl_context_set_required_version(context, 3, 2);
        error = nullptr;
        bool realized = gdk_gl_context_realize(context, &error);
        if (error) g_error_free(error);
        if (!realized) { g_object_unref(context); context = nullptr; return false; }
        g_signal_connect_object(gdk_window_get_frame_clock(window), "after-paint",
            G_CALLBACK(+[](GdkFrameClock *, gpointer) { gdk_gl_context_clear_current(); }),
            context, G_CONNECT_AFTER);
    }
    const int scale = gdk_window_get_scale_factor(window);
    const int next_width = gdk_window_get_width(window) * scale;
    const int next_height = gdk_window_get_height(window) * scale;
    if (width != next_width || height != next_height) {
        width = next_width;
        height = next_height;
        if (native_window) wl_egl_window_resize(native_window, width, height, 0, 0);
    }
    if (!make_current() ||
        epoxy_gl_version() < (epoxy_is_desktop_gl() ? 32 : 30) ||
        !epoxy_has_gl_extension("GL_OES_EGL_image")) return false;
    display = eglGetCurrentDisplay();
    if (!native_window) {
        EGLint id, count, min_interval;
        EGLConfig config;
        if (!eglQueryContext(display, eglGetCurrentContext(), EGL_CONFIG_ID, &id)) return false;
        const EGLint attributes[] = { EGL_CONFIG_ID, id, EGL_NONE };
        if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1) return false;
        if (!eglGetConfigAttrib(display, config, EGL_MIN_SWAP_INTERVAL, &min_interval) || min_interval > 0)
            return false;
        surface = gdk_wayland_window_get_wl_surface(window);
        if (!surface) return false;
        native_window = wl_egl_window_create(surface, next_width, next_height);
        if (!native_window) return false;
        target = eglCreateWindowSurface(display, config, (EGLNativeWindowType)native_window, nullptr);
        if (target == EGL_NO_SURFACE || !make_current() || !eglSwapInterval(display, 0)) return false;
    }
    wl_surface_set_buffer_scale(surface, scale);
    EGLImageKHR image = buffer.import(display);
    if (image == EGL_NO_IMAGE_KHR) return false;
    if (!texture) {
        glGenTextures(1, &texture);
        glGenFramebuffers(1, &framebuffer);
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    eglDestroyImageKHR(display, image);
    if (glGetError() != GL_NO_ERROR) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, w, h, 0, height, w, height-h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return glGetError() == GL_NO_ERROR;
}

bool CaptureGL::draw(GtkWidget *widget, const CapturePixmap &pixmap)
{
    if (disabled) return false;
    GdkWindow *window = gtk_widget_get_window(widget);
    if (!window || !GDK_IS_WAYLAND_WINDOW(window)) return false;
    if (surface && surface != gdk_wayland_window_get_wl_surface(window)) reset();
    GdkGLContext *previous = gdk_gl_context_get_current();
    if (previous) g_object_ref(previous);
    bool success = read(pixmap, window);
    if (success) success = eglSwapBuffers(display, target);
    if (!success) {
        reset();
        disabled = true;
    }
    if (previous) { gdk_gl_context_make_current(previous); g_object_unref(previous); }
    else gdk_gl_context_clear_current();
    return success;
}

static void xw_size(HWND hwnd);

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
    GtkWidget *active_toplevel = nullptr;
    HWND       hwnd        = nullptr;
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

static Capture *g_active_capture = nullptr;

static void set_active_capture(Capture *c)
{
    g_active_capture = c;
    if (g_wm) g_wm->set_active_window(c ? c->parent_win : XCB_NONE);
}

struct XWaylandBridge {
    xcb_connection_t *connection = nullptr;
    xcb_window_t parent = 0;
    Capture *cap    = nullptr;
    bool     placed = false;
    RECT     last_size_pos = {0,0,0,0};
    bool     has_last_size_pos = false;
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

static void raise_window(xcb_window_t window)
{
    for (const auto &kv : g_captures) {
        const Capture *c = kv.second;
        if (kv.first != c->parent_win) continue;
        for (const auto &p : c->popups)
            if (p.visible) return;
    }
    swell_xcb_raise(wm_connection(), window);
}

static void forward_motion(Capture *c, double wx, double wy)
{
    resume_pointer();
    raise_window(c->parent_win);
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

bool xw_bridge_raise_modals()
{
    bool any = false;
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c || kv.first != c->parent_win) continue;
        if (c->modals.empty()) continue;
        for (auto &md : c->modals)
            if (md.gtk_win)
            {
                raise_window(md.x11_win);
                gtk_window_present(GTK_WINDOW(md.gtk_win));
                gtk_window_set_keep_above(GTK_WINDOW(md.gtk_win), TRUE);
                any = true;
            }
    }
    if (any && wm_connection()) xcb_flush(wm_connection());
    return any;
}

static bool on_button_press(GtkWidget *widget, GdkEventButton *e, gpointer data)
{
    DEBUG_PRINT("[DNDX] widget button PRESS btn=%d\n", e->button);
    Capture *c = (Capture*)data;
    if (!c || !c->connection) return false;
    if (xw_bridge_raise_modals()) return true;
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

static void on_toplevel_active(GObject *obj, GParamSpec *, gpointer data)
{
    Capture *c = (Capture*)data;
    if (!c) return;

    gboolean active = FALSE;
    g_object_get(obj, "is-active", &active, NULL);

    if (active) {
        if (g_active_capture && g_active_capture != c && !g_active_capture->modals.empty())
            return;

        set_active_capture(c);
        if (c->widget) {
            GdkWindow *gw = gtk_widget_get_window(c->widget);
            if (gw) {
                GdkCursor *cur = gdk_cursor_new_from_name(gdk_window_get_display(gw), "default");
                gdk_window_set_cursor(gw, cur);
                if (cur) g_object_unref(cur);
            }
        }
        if (c->connection && c->parent_win) {
            raise_window(c->parent_win);
            xcb_flush(wm_connection());
        }
        xw_bridge_raise_modals();
    } else if (g_active_capture == c) {
        if (!c->modals.empty()) return;
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
                          GDK_KEY_PRESS_MASK |
                          GDK_KEY_RELEASE_MASK |
                          GDK_SCROLL_MASK);

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

    xcb_composite_redirect_window(connection, plugin_win, XCB_COMPOSITE_REDIRECT_AUTOMATIC);

    update_pixmap(c);

    c->damage = create_damage(plugin_win);

    xcb_flush(connection);

    register_capture(c);
    set_active_capture(c);
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

static bool has_input_popup(const Capture *c)
{
    for (const auto &p : c->popups)
        if (p.visible && popup_accepts_input(c->connection, p.x11_win)) return true;
    return false;
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
    gtk_widget_add_events(win, GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);

    GdkScreen *screen = gdk_screen_get_default();
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual != nullptr) {
        gtk_widget_set_visual(win, visual);
    }
    gtk_widget_set_app_paintable(win, TRUE);

    GtkWidget *widget = c->modals.empty() ? c->widget : c->modals.back().draw;
    GtkWidget *top = widget ? gtk_widget_get_toplevel(widget) : nullptr;
    if (top && GTK_IS_WINDOW(top))
        gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(top));

    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);

    const GdkRectangle bounds = c->modals.empty() ? c->monitor_bounds : capture_monitor(c->connection, widget);
    const int scale = swell_gdk_widget_scale(widget);
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

static void refresh_gtk_offset(Capture *c)
{
    if (!c || !c->connection || !c->widget) return;

    GtkWidget *widget = c->modals.empty() ? c->widget : c->modals.back().draw;
    xcb_window_t origin = c->modals.empty() ? c->parent_win : c->modals.back().x11_win;
    int px = 0, py = 0;
    swell_xcb_translate(c->connection, origin,
                          swell_xcb_screen(c->connection)->root, 0, 0, &px, &py);

    GdkWindow *ww = gtk_widget_get_window(widget);
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
    if (has_input_popup(m->cap)) return TRUE;
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
    raise_window(m->x11_win);
    const int scale = swell_gdk_widget_scale(widget);
    int rx = 0, ry = 0;
    swell_xcb_translate(wm_connection(), m->x11_win, swell_xcb_screen(m->cap->connection)->root,
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
            if (c->popup_canvas && gtk_window_get_transient_for(GTK_WINDOW(c->popup_canvas)) == GTK_WINDOW(w))
                while (!c->popups.empty()) canvas_remove_popup(c, c->popups.back().x11_win);
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
    bool any = xw_bridge_raise_modals();
    for (auto &kv : g_captures)
    {
        Capture *c = kv.second;
        if (!c || kv.first != c->parent_win) continue;
        if (!c->popups.empty()){
            dismiss_popups(c);
            any = true;
        }
    }
    return any;
}

static bool capture_owned_by(const Capture *c, HWND hwnd)
{
    HWND owner = c->hwnd;
    while (owner && owner != hwnd) owner = owner->m_parent ? owner->m_parent : owner->m_owner;
    return owner != nullptr;
}

bool xw_bridge_popup_blocks_close(HWND hwnd)
{
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (kv.first != c->parent_win) continue;
        if (capture_owned_by(c, hwnd) && has_input_popup(c)) return true;
    }
    return false;
}

bool xw_bridge_swell_on_gdk_delete_release(HWND hwnd)
{
    bool any = false;
    for (auto &kv : g_captures) {
        Capture *c = kv.second;
        if (!c || kv.first != c->parent_win) continue;
        if (!capture_owned_by(c, hwnd)) continue;
        if (!c->popups.empty()) {
            any = true;
            dismiss_popups(c);
            while (!c->popups.empty()) canvas_remove_popup(c, c->popups.back().x11_win);
        }
        if (!c->modals.empty() && c->connection) {
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

static void xw_fixed_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    gtk_widget_set_allocation(widget, allocation);
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *p = children; p; p = p->next) {
        GtkWidget *child = GTK_WIDGET(p->data);
        if (!gtk_widget_get_visible(child)) continue;
        GtkAllocation rect;
        gtk_container_child_get(GTK_CONTAINER(widget), child, "x", &rect.x, "y", &rect.y, nullptr);
        rect.width = allocation->width - rect.x;
        rect.height = allocation->height - rect.y;
        gtk_widget_set_child_visible(child, rect.width > 0 && rect.height > 0);
        if (rect.width <= 0 || rect.height <= 0) continue;
        if (!GTK_IS_FIXED(child)) {
            GtkRequisition size;
            gtk_widget_get_preferred_size(child, &size, nullptr);
            rect.width = min(rect.width, size.width);
            rect.height = min(rect.height, size.height);
        }
        rect.x += allocation->x;
        rect.y += allocation->y;
        gtk_widget_size_allocate(child, &rect);
    }
    g_list_free(children);
}

static void xw_fixed_class_init(XwFixedClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->get_preferred_width = xw_fixed_get_preferred_size;
    widget_class->get_preferred_height = xw_fixed_get_preferred_size;
    widget_class->size_allocate = xw_fixed_size_allocate;
}

static void xw_fixed_init(XwFixed *)
{
}

static void detach_bridge_widgets(GtkWidget *widget);

static GtkWidget* xw_ensure_embed_widget(HWND h)
{
    if (!h) return nullptr;

    if (h->m_oswidget) {
        GtkWidget *top = (GtkWidget*)h->m_oswidget;
        if (!GTK_IS_WINDOW(top)) return top;
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

    GtkWidget *fixed = GTK_WIDGET(g_object_new(xw_fixed_get_type(), NULL));
    gtk_fixed_put(GTK_FIXED(parent_widget), fixed, x, y);
    gtk_widget_show(fixed);

    g_object_set_data(G_OBJECT(fixed), "xw-embed-hwnd", h);
    g_object_set_data_full(G_OBJECT(parent_widget), "xw-active-embed-fixed", g_object_ref(fixed),
                          [](gpointer widget) { g_object_unref(widget); });
    return fixed;
}

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
    XWaylandBridge *bs = (XWaylandBridge*)hwnd->m_private_data;

    if (!bs->placed) xw_size(hwnd);

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

static void xw_destroy(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data) return;
    XWaylandBridge *bs = (XWaylandBridge*)hwnd->m_private_data;
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

static void xw_size(HWND hwnd)
{
    if (!hwnd || !hwnd->m_private_data || !hwnd->m_oswidget) return;
    XWaylandBridge *bs = (XWaylandBridge*)hwnd->m_private_data;

    GtkWidget *container = xw_ensure_embed_widget(hwnd->m_parent);
    if (!container || !GTK_IS_FIXED(container)) {
        bs->placed = false;
        return;
    }
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

    GtkWidget *old_parent = gtk_widget_get_parent(hwnd->m_oswidget);
    if (old_parent != container) {
        if (old_parent) gtk_container_remove(GTK_CONTAINER(old_parent), hwnd->m_oswidget);
        gtk_fixed_put(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
        gtk_widget_show(hwnd->m_oswidget);
    } else {
        gtk_fixed_move(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
    }
    gtk_widget_set_size_request(hwnd->m_oswidget, w, h);
    bs->placed = GTK_IS_WINDOW(gtk_widget_get_toplevel(hwnd->m_oswidget));

    if (size_changed) {
        bs->last_size_pos = r;
        bs->has_last_size_pos = true;
    }

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

    XWaylandBridge *bs = new XWaylandBridge();
    bs->connection   = connection;
    bs->parent = container;
    hwnd->m_private_data = (INT_PTR)bs;
    g_signal_connect(draw_area, "notify::scale-factor", G_CALLBACK(xw_scale_changed), hwnd);

    *wref = (void*)(uintptr_t)container;

    xw_size(hwnd);

    SetTimer(hwnd, 1, 100, NULL);
    return hwnd;
}

bool xw_bridge_forward_key(HWND hwnd, int keycode, int state, bool is_press)
{
    if (!hwnd || !hwnd->m_private_data) return false;
    XWaylandBridge *bs = (XWaylandBridge*)hwnd->m_private_data;
    Capture *c = bs->cap;
    if (!c || !c->connection) return false;

    xcb_window_t target = (c->root_popup && !c->popups.empty()) ? c->root_popup
                  : (c->gui_win ? c->gui_win : c->plugin_win);
    if (!target) return false;

    forward_key(c->connection, target, keycode, state, is_press);
    return true;
}

bool xw_bridge_key_event(GdkEventKey *e)
{
    for (const auto &kv : g_captures) {
        Capture *c = kv.second;
        if (kv.first != c->parent_win) continue;
        if ((c->widget && gtk_widget_get_window(c->widget) == e->window) ||
            (c->popup_canvas && gtk_widget_get_window(c->popup_canvas) == e->window))
            return xw_bridge_forward_key(c->hwnd, e->hardware_keycode,
                                         e->state, e->type == GDK_KEY_PRESS);
    }
    return false;
}
