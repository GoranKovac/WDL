#pragma once

#include "swell.h"
#include <gdk/gdk.h>

void swell_gdk_expose(GdkEventExpose *event, cairo_t *cr = NULL, bool background = false);
void swell_gdk_close(HWND hwnd);
void swell_gdk_configure(GdkEventConfigure *event);
void swell_gdk_drag_enter(int x, int y);
void swell_gdk_receive_drop(HWND hwnd, GdkEventDND *event);
