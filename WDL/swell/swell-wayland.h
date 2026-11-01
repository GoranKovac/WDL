#pragma once

#include "swell.h"
#include <gdk/gdk.h>

void swell_wayland_init();
bool swell_wayland_create_window(HWND hwnd, GdkWindow *transient_for);
void swell_wayland_forget_window(HWND hwnd);
void swell_wayland_set_geometry_hints(HWND hwnd, const GdkGeometry *hints);
bool swell_wayland_needs_manual_titlebar(HWND hwnd);
void swell_wayland_paint_manual_titlebar(HWND hwnd, GdkWindow *window, cairo_t *cr);
void swell_wayland_motion_event(const GdkEventMotion *event);
bool swell_wayland_button_event(HWND hwnd, const GdkEventButton *event);

extern GdkWindow *s_last_hover_oswindow;
extern int g_swell_wayland_title_h;
extern int SWELL_WAYLAND_BORDER_WIDTH;
