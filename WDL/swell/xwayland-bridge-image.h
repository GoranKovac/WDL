#pragma once

#include <cairo.h>
#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xcb_image.h>

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
