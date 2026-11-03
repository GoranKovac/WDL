#pragma once

#include "swell.h"
#include <gdk/gdk.h>

cairo_surface_t *swell_gdk_paint(HWND hwnd, const GdkRectangle &area);
cairo_surface_t *swell_gdk_surface(HWND hwnd);
bool swell_gdk_size_hints(HWND hwnd, const RECT *fixed, GdkGeometry *hints);
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
  void (*update_window)(HWND hwnd, const RECT *rect);
  void (*invalidate)(HWND hwnd, const RECT *rect);
  void (*get_window_rect)(HWND hwnd, RECT *rect);
  void (*resize_window)(GdkWindow *window, int flags, RECT rect, bool mapped);
  void (*set_size_hints)(HWND hwnd, const RECT *fixed);
  void (*activate)(bool active);
  const char *(*window_manager_name)();
  bool (*create_window)(HWND hwnd, GdkWindow *transient_for);
  void (*show_window)(HWND hwnd, bool raise, bool modal, guint32 user_time);
  void (*destroy_window)(HWND hwnd);
  bool (*close_window)(HWND hwnd);
  bool (*focus_window)(HWND hwnd, bool allow_force, bool override_redirect);
  void (*set_decorations)(HWND hwnd);
  void (*set_cursor)(GdkWindow *window, GdkCursor *cursor, bool grabbed);
  HWND (*create_bridge)(HWND parent, void **window, const RECT *rect);
  void (*invalidate_focus)(GdkWindow *window);
  bool relative_mouse;
  bool (*get_viewport)(RECT *rect);
  void (*monitor_geometry)(int index, bool work, GdkRectangle *rect);
  void (*scaling_init)(bool no_auto_hidpi);
};

extern const SWELL_GdkBackend swell_gdk_x11;
#ifdef SWELL_TARGET_WAYLAND
extern const SWELL_GdkBackend swell_gdk_wayland;
bool swell_wayland_init_display();
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

void swell_recalcMinMaxInfo(HWND hwnd);

void swell_gdk_set_decorations(HWND hwnd);

void swell_gdk_monitor_geometry(int index, bool work, GdkRectangle *rect);

bool SWELL_load_gtk();
