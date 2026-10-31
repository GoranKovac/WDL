#include "xwayland-bridge-display.h"
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <glib.h>

#include <wayland-client.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

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
                // xwayland_shell_v1: wlroots compositors (e.g. mango) advertise it to
                // every client but kill any non-compositor Xwayland that binds it.
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

pid_t xw_spawn_xwayland(char *display_name, size_t size)
{
    if (size < 3) return -1;
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
    int ready[2];
    if (pipe2(ready, O_CLOEXEC) < 0) {
        close(sockets[1]);
        return -1;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        close(ready[0]);
        char ready_fd[32];
        snprintf(ready_fd, sizeof(ready_fd), "%d", ready[1]);
        if (fcntl(ready[1], F_SETFD, 0) < 0) _exit(1);
        close(sockets[0]);
        close(server);
        char socket_name[32];
        snprintf(socket_name, sizeof(socket_name), "%d", sockets[1]);
        if (fcntl(sockets[1], F_SETFD, 0) < 0 || setenv("WAYLAND_SOCKET", socket_name, 1)) _exit(1);
        execl("/usr/bin/Xwayland", "Xwayland", "-displayfd", ready_fd,
              "-rootless", "-nolisten", "tcp", (char*)nullptr);
        _exit(1);
    }
    close(ready[1]);
    close(sockets[1]);
    if (pid > 0) {
        std::thread to_server, to_client;
        try {
            to_server = std::thread(relay_wayland, connection, false);
            to_client = std::thread(relay_wayland, connection, true);
        } catch (...) {
            close(ready[0]);
            connection->stop();
            if (to_server.joinable()) to_server.join();
            if (to_client.joinable()) to_client.join();
            kill(pid, SIGTERM);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
            return -1;
        }
        to_server.detach();
        to_client.detach();
    }
    display_name[0] = ':';
    size_t length = 1;
    const gint64 deadline = g_get_monotonic_time() + 10000000;
    while (pid > 0 && length < size) {
        const int remaining = (deadline - g_get_monotonic_time()) / 1000;
        if (remaining <= 0) break;
        pollfd fd = { ready[0], POLLIN, 0 };
        const int result = poll(&fd, 1, remaining);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        const ssize_t count = read(ready[0], display_name + length, size - length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        length += count;
        if (display_name[length - 1] == '\n') {
            display_name[--length] = 0;
            if (length <= 1 || strspn(display_name + 1, "0123456789") != length - 1) break;
            close(ready[0]);
            return pid;
        }
    }
    close(ready[0]);
    if (pid > 0) {
        kill(pid, SIGTERM);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
    }
    return -1;
}
