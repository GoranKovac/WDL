#pragma once

#include "swell.h"
#include <gdk/gdk.h>

HWND swell_x11_create_bridge(HWND parent, void **window, const RECT *rect);
bool swell_x11_drag_event(GdkEvent *event);
void swell_x11_drag_check_timeout();

guint32 swell_x11_get_desktop(GdkWindow *window);
void swell_x11_move_to_desktop(GdkWindow *window, guint32 desktop);
bool swell_x11_has_focus(GdkWindow *window);
void swell_x11_force_focus(GdkWindow *window);
void swell_x11_set_grab_cursor(GdkWindow *window, GdkCursor *cursor);
bool swell_x11_has_selection_owner(GdkDisplay *display, GdkAtom selection);
