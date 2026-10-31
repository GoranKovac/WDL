#pragma once

#include <gtk/gtk.h>
#include "xwayland-bridge-image.h"

class CaptureGL {
    GdkGLContext *context = nullptr;
    xcb_connection_t *connection = nullptr;
    unsigned int fence = 0;
    void *pending_read = nullptr;
    unsigned int textures[2] = {}, framebuffers[2] = {};
    int width = 0, height = 0, depth = 0;
    bool disabled = false;

    bool make_current();
    bool read(const CapturePixmap &pixmap, GdkWindow *window);

public:
    ~CaptureGL() { reset(); }
    void reset();
    bool draw(GtkWidget *widget, cairo_t *cr, const CapturePixmap &pixmap, bool refresh);
};
