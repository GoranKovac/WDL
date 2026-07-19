#pragma once

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/XTest.h>
#include <cairo-xlib.h>

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
#include <X11/extensions/Xdamage.h>
#include "xwayland-bridge-wm.h"
#include "swell.h"
#include "swell-internal.h"

void init_private_xwayland();
HWND xw_bridge_create(
    HWND viewpar,
    void **wref,
    const RECT *r,
    const char* bridge_class_name
);
bool xw_bridge_forward_key(HWND hwnd, int keycode, int state, bool is_press);
// Deliver a key to an open plugin modal dialog and report that it was consumed.
// MUST be checked before SWELL dispatches a key into REAPER: while a Wine modal is up
// the plugin sits in a nested message loop and cannot answer yabridge, so any
// host->plugin call blocks REAPER's UI thread in recv() and hangs it outright.
bool xw_bridge_forward_key_to_modal(int keycode, int state, bool is_press);
bool xw_bridge_swell_on_button_event_escape();
bool xw_bridge_swell_on_gdk_delete_release();
extern XWaylandWM *g_wm;
extern Display *g_wm_dpy;
