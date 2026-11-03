#pragma once

#include "swell.h"
#include <gdk/gdk.h>

void swell_gdk_expose(GdkEventExpose *event, cairo_t *cr = NULL, bool background = false);
void swell_gdk_close(HWND hwnd);
void swell_gdk_configure(GdkEventConfigure *event);
void swell_gdk_drag_enter(int x, int y);
void swell_gdk_receive_drop(HWND hwnd, GdkEventDND *event);

struct SWELL_GdkBackend
{
  void (*event)(GdkEvent *event);
  void (*mouse_event)(GdkEvent *event);
  void (*run_events)();
  void (*get_cursor_pos)(POINT *point);
  WORD (*get_async_key_state)(int key);
};

extern const SWELL_GdkBackend swell_gdk_x11;
#ifdef SWELL_TARGET_WAYLAND
extern const SWELL_GdkBackend swell_gdk_wayland;
#endif
extern const SWELL_GdkBackend *swell_gdk_backend;
extern GdkEvent *swell_gdk_current_event;
extern HWND swell_gdk_drag_source;
extern DWORD swell_lastMessagePos;

void swell_gdk_dispatch_event(GdkEvent *event);
void swell_gdk_prepare_key_event();
void swell_gdk_key_event(GdkEventKey *event);
void swell_gdk_motion_event(GdkEventMotion *event);
void swell_gdk_button_event(GdkEventButton *event);
void swell_gdk_scroll_event(GdkEventScroll *event);
WORD swell_gdk_get_async_key_state(int key);
HANDLE swell_gdk_urilist_to_dropfiles(const POINT *point, const guchar *data, gint size);

LRESULT SWELL_SendMouseMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, int root_x, int root_y);
