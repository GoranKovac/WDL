#pragma once

#include <gtk/gtk.h>
#include <epoxy/egl.h>
#include "xwayland-bridge-image.h"

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
