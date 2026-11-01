#pragma once

#include <gdk/gdk.h>
#include "swell.h"

void init_private_xwayland();
void xw_bridge_detach_children(HWND hwnd);
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
bool xw_bridge_swell_on_gdk_delete_release(HWND hwnd);
bool xw_bridge_has_pending_draw();
bool xw_bridge_draw(GdkWindow *window);
void xw_clip_capture_regions(GdkWindow *window, cairo_region_t *region);
