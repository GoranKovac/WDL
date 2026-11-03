#pragma once

#include "swell.h"
#include <gdk/gdk.h>
#include "../heapbuf.h"

class WDL_FastString;
struct SWELL_GdkDrag;

cairo_surface_t *swell_gdk_paint(HWND hwnd, const GdkRectangle &area);
cairo_surface_t *swell_gdk_surface(HWND hwnd);
bool swell_gdk_size_hints(HWND hwnd, const RECT *fixed, GdkGeometry *hints);
void swell_gdk_close(HWND hwnd);
void swell_gdk_configure(GdkEventConfigure *event);
void swell_gdk_drag_enter(int x, int y);

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
  void (*hide_window)(HWND hwnd);
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
  void (*clipboard_open)();
  HANDLE (*clipboard_read)(GdkAtom type);
  bool (*clipboard_claim)();
  void (*clipboard_publish)();
  bool (*clipboard_text)(GdkAtom atom);
  void (*drag_files)(HWND hwnd, const SWELL_GdkDrag &data);
  void (*begin_drag)(HWND source);
  HWND (*window_from_point)(POINT point);
  bool reuse_popup;
  void (*end_modal)(HWND hwnd);
};

extern const SWELL_GdkBackend swell_gdk_x11;
void swell_x11_init();
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

struct SWELL_GdkClipboard
{
  HWND owner;
  HANDLE read_data; // currently retrieving data/format via clipboard_read() (GetClipboardData() or EnumClipboardFormats(0))
  GdkAtom read_type;
  // clipboard data added by SetClipboardData():
  WDL_TypedBuf<GdkAtom> formats; // stored as separate lists so we can easily pass to properties
  WDL_TypedBuf<HANDLE> contents;
};

extern SWELL_GdkClipboard swell_gdk_clipboard;
GdkAtom swell_gdk_utf8_atom();
GdkAtom swell_gdk_targets_atom();
GdkAtom swell_gdk_atom_atom();
GdkAtom swell_gdk_urilist_atom();
HANDLE *swell_gdk_clipboard_find(GdkAtom atom);
void swell_gdk_clipboard_format(GdkAtom format, HANDLE handle, WDL_FastString &str);
void swell_gdk_encode_uri(WDL_FastString *str, const char *path);

struct SWELL_GdkDrag
{
  const char **files;
  int count;
  const char *filename;
  void (*callback)(const char *);
};
