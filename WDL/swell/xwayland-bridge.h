#pragma once

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/XTest.h>
#include <cairo-xlib.h>

#include "swell.h"
#include "swell-internal.h"

#include <glib.h>

#include <map>
#include <vector>
#include <algorithm>
#include <sys/wait.h>
#include <signal.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <GL/glx.h>

#include <sys/prctl.h>

extern Display *s_private_xwayland_display;
void init_private_xwayland();
HWND xw_bridge_create(
    HWND viewpar,
    void **wref,
    const RECT *r,
    const char* bridge_class_name
);
bool xw_forward_key(HWND hwnd, int keycode, int state, bool is_press);
