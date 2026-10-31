#pragma once

#include <gtk/gtk.h>
#include <X11/Xlib.h>

class CaptureGL {
    GdkGLContext *context = nullptr;
    Display *dpy = nullptr;
    unsigned int fence = 0;
    void *pending_read = nullptr;
    unsigned int textures[2] = {}, framebuffers[2] = {};
    int width = 0, height = 0, depth = 0;
    bool disabled = false;

    bool make_current();
    bool read(Display *display, Pixmap pixmap, Visual *visual, GdkWindow *window, int w, int h);

public:
    ~CaptureGL() { reset(); }
    void reset();
    bool draw(GtkWidget *widget, cairo_t *cr, Display *display, Pixmap pixmap,
              Visual *visual, int w, int h, bool refresh);
};
