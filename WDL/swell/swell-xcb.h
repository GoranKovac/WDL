#pragma once

#include <xcb/xcb.h>
#include <stdlib.h>

static inline xcb_screen_t *swell_xcb_screen(xcb_connection_t *connection)
{
    return xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
}

static inline void swell_xcb_sync(xcb_connection_t *connection)
{
    free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr));
}

static inline bool swell_xcb_translate(xcb_connection_t *connection, xcb_window_t source,
    xcb_window_t destination, int x, int y, int *out_x, int *out_y)
{
    xcb_translate_coordinates_reply_t *reply = xcb_translate_coordinates_reply(connection,
        xcb_translate_coordinates(connection, source, destination, x, y), nullptr);
    if (!reply) return false;
    *out_x = reply->dst_x;
    *out_y = reply->dst_y;
    const bool same_screen = reply->same_screen;
    free(reply);
    return same_screen;
}

static inline void swell_xcb_raise(xcb_connection_t *connection, xcb_window_t window)
{
    const uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
}

static inline void swell_xcb_move_resize(xcb_connection_t *connection, xcb_window_t window,
    int x, int y, int width, int height)
{
    const uint32_t values[] = { (uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height };
    xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
        XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
}
