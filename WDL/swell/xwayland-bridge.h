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
bool xw_bridge_forward_key_to_modal(int keycode, int state, bool is_press);
bool xw_bridge_swell_on_button_event_escape();
bool xw_bridge_popup_blocks_close(HWND hwnd);
bool xw_bridge_swell_on_gdk_delete_release(HWND hwnd);
bool xw_bridge_has_pending_draw();
bool xw_bridge_draw(GdkWindow *window);
void xw_clip_capture_regions(GdkWindow *window, cairo_region_t *region);
