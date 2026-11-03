/* Cockos SWELL (Simple/Small Win32 Emulation Layer for Linux/OSX)
   Copyright (C) 2006 and later, Cockos, Inc.

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.


  */


#if defined(SWELL_TARGET_WAYLAND) && !defined(SWELL_PROVIDED_BY_APP)

#include <gdk/gdkwayland.h>
#include <gio/gdesktopappinfo.h>
#include "swell.h"
#include "swell-internal.h"
#include "swell-gdk.h"
#include "swell-wayland.h"
#include "swell-gdk-scale.h"
#include "swell-xwayland-bridge.h"

#define PRIVATE_SIZEPIN 2

static void swell_wayland_set_geometry_hints(HWND hwnd, const GdkGeometry *hints);
static void swell_wayland_paint_manual_titlebar(HWND hwnd, GdkWindow *window, cairo_t *cr);

static HWND g_wayland_hovered_hwnd;

static gboolean swell_wayland_handle_hover_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  HWND hwnd = (HWND)user_data;
  if (!hwnd) return FALSE;

  if (event->type == GDK_ENTER_NOTIFY)
  {
    g_wayland_hovered_hwnd = hwnd;
  }
  else if (event->type == GDK_LEAVE_NOTIFY)
  {
    if (g_wayland_hovered_hwnd == hwnd)
      g_wayland_hovered_hwnd = NULL;
  }
  return FALSE;
}

int g_swell_wayland_title_h;
#define SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W 36
int SWELL_WAYLAND_BORDER_WIDTH = 2;

bool swell_is_wayland()
{
  return GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
}

bool swell_wayland_init_display()
{
  if (!swell_is_wayland()) return true;
  if (!SWELL_load_gtk()) return false;
  swell_gdk_backend = &swell_gdk_wayland;
  init_private_xwayland();
  return true;
}

static void swell_wayland_init_application_id()
{
  if (!g_swell_appname || !GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) return;

  GList *apps = g_app_info_get_all();
  for (GList *p = apps; p; p = p->next)
  {
    if (!G_IS_DESKTOP_APP_INFO(p->data)) continue;
    const char *wmclass = g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(p->data));
    const char *id = g_app_info_get_id(G_APP_INFO(p->data));
    if (wmclass && !strcmp(wmclass, g_swell_appname) && id && g_str_has_suffix(id, ".desktop"))
    {
      char *app_id = g_strndup(id, strlen(id) - strlen(".desktop"));
      g_set_prgname(app_id);
      g_free(app_id);
      break;
    }
  }
  for (GList *p = apps; p; p = p->next) g_object_unref(p->data);
  g_list_free(apps);
}

static void swell_wayland_init_titlebar_height()
{
  GdkDisplay *disp = gdk_display_get_default();
  g_swell_wayland_title_h = GDK_IS_WAYLAND_DISPLAY(disp) && !gdk_wayland_display_prefers_ssd(disp) ? 28 : 0;
}

static void swell_wayland_suppress_csd_shadow_once()
{
  static bool done = false;
  if (done || !g_swell_wayland_title_h) return;
  done = true;

  GtkCssProvider *provider = gtk_css_provider_new();
  const char *css =
    "window.swell decoration {"
    "  box-shadow: none;"
    "  border: none;"
    "  border-radius: 0;"
    "  margin: 0;"
    "  padding: 0;"
    "}";
  gtk_css_provider_load_from_data(provider, css, -1, NULL);
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                             GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

bool swell_wayland_needs_manual_titlebar(HWND hwnd)
{
  return hwnd && g_swell_wayland_title_h && (hwnd->m_style & WS_CAPTION);
}

static void swell_wayland_init()
{
  swell_wayland_init_application_id();
  swell_wayland_init_titlebar_height();
  swell_wayland_suppress_csd_shadow_once();
}

static void expose(GdkEventExpose *event, cairo_t *draw_ctx = NULL, bool background = false)
{
  HWND hwnd = swell_oswindow_to_hwnd(event->window);
  if (!hwnd) return;
  const bool use_background = hwnd->m_oswidget && gtk_widget_get_opacity(hwnd->m_oswidget) == 1.0;
  if (draw_ctx && use_background && swell_gdk_current_event &&
      swell_gdk_current_event->type == GDK_EXPOSE &&
      swell_gdk_current_event->any.window == event->window && gdk_cairo_get_drawing_context(draw_ctx))
  {
    if (swell_wayland_needs_manual_titlebar(hwnd))
      swell_wayland_paint_manual_titlebar(hwnd, event->window, draw_ctx);
    return;
  }
  if (background)
  {
    gdk_window_set_background_pattern(event->window, NULL);
    if (!use_background) return;
  }

  const int scale = swell_gdk_window_scale(event->window);
  const bool titlebar = swell_wayland_needs_manual_titlebar(hwnd);
  const int bw = titlebar ? SWELL_WAYLAND_BORDER_WIDTH * scale : 0;
  const int title_h = titlebar ? g_swell_wayland_title_h * scale : 0;
  GdkRectangle area = { event->area.x * scale - bw, event->area.y * scale - title_h - bw,
                        event->area.width * scale, event->area.height * scale };
  hwnd->Retain();
  cairo_surface_t *surface = swell_gdk_paint(hwnd, area);
  if (surface)
  {
    if (background)
    {
      cairo_surface_mark_dirty(surface);
      cairo_pattern_t *pattern = cairo_pattern_create_for_surface(surface);
      cairo_matrix_t matrix;
      cairo_matrix_init(&matrix, scale, 0, 0, scale, -bw, -(title_h + bw));
      cairo_pattern_set_matrix(pattern, &matrix);
      gdk_window_set_background_pattern(event->window, pattern);
      cairo_pattern_destroy(pattern);
    }
    else
    {
      cairo_save(draw_ctx);
      cairo_scale(draw_ctx, 1.0 / scale, 1.0 / scale);
      cairo_set_source_surface(draw_ctx, surface, bw, title_h + bw);
      cairo_paint(draw_ctx);
      cairo_restore(draw_ctx);
    }
    cairo_surface_destroy(surface);
  }
  if (!hwnd->m_hashaddestroy && !background && titlebar)
    swell_wayland_paint_manual_titlebar(hwnd, event->window, draw_ctx);
  hwnd->Release();
}

static void configure(GdkEventConfigure *event)
{
  HWND hwnd = swell_oswindow_to_hwnd(event->window);
  if (!hwnd || event->width != gdk_window_get_width(event->window) ||
      event->height != gdk_window_get_height(event->window)) return;
  GdkEventConfigure pixels = *event;
  const int scale = swell_gdk_window_scale(event->window);
  pixels.x *= scale;
  pixels.y *= scale;
  pixels.width *= scale;
  pixels.height *= scale;
  if (swell_wayland_needs_manual_titlebar(hwnd))
  {
    const int bw = SWELL_WAYLAND_BORDER_WIDTH * scale;
    const int title_h = g_swell_wayland_title_h * scale;
    pixels.x += bw;
    pixels.y += title_h + bw;
    pixels.width -= bw * 2;
    pixels.height -= title_h + bw * 2;
  }
  swell_gdk_configure(&pixels);
}

static void swell_wayland_scale_changed(GtkWidget *widget, GParamSpec *, gpointer data)
{
  HWND hwnd = (HWND)data;
  if (!hwnd->m_oswindow || !gtk_widget_get_mapped(widget)) return;
  GdkEventConfigure cfg = {};
  cfg.window = hwnd->m_oswindow;
  gdk_window_get_geometry(cfg.window, &cfg.x, &cfg.y, &cfg.width, &cfg.height);
  hwnd->Retain();
  configure(&cfg);
  if (!hwnd->m_hashaddestroy) InvalidateRect(hwnd, NULL, FALSE);
  hwnd->Release();
}

static HWND s_wayland_active_tooltip;
static SWELL_OSWINDOW s_last_hover_oswindow;
bool PopupMenuIsActive();

static SWELL_OSWINDOW s_armed_drag_oswindow;
static guint s_armed_drag_button;
static gint s_armed_drag_root_x, s_armed_drag_root_y;
static guint32 s_armed_drag_time;

static gboolean swell_wayland_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
  HWND hwnd = (HWND)data;
  GdkWindow *window = gtk_widget_get_window(widget);
  if (window != hwnd->m_oswindow || !gtk_cairo_should_draw_window(cr, window)) return FALSE;
  GdkEventExpose exp = {};
  exp.window = window;
  cairo_save(cr);
  cairo_region_t *region = gdk_window_get_visible_region(window);
  xw_clip_capture_regions(window, region);
  gdk_window_set_opaque_region(window, gtk_widget_get_opacity(widget) == 1.0 ? region : NULL);
  gdk_cairo_region(cr, region);
  cairo_region_destroy(region);
  cairo_clip(cr);
  if (gdk_cairo_get_clip_rectangle(cr, &exp.area)) expose(&exp, cr);
  cairo_restore(cr);
  return FALSE;
}

static bool swell_wayland_create_window(HWND hwnd, GdkWindow *transient_for)
{
  const RECT r = hwnd->m_position;
  bool is_popup_menu = (hwnd->m_classname && strcmp(hwnd->m_classname, "__SWELL_MENU") == 0);
  bool is_tooltip = (!hwnd->m_parent && !hwnd->m_owner && (hwnd->m_style & WS_CHILD) && (!hwnd->m_title.Get() || !hwnd->m_title.Get()[0]));

  bool is_interactive_popup = (hwnd->m_style & WS_CHILD) && hwnd->m_owner;
  if (is_interactive_popup) is_popup_menu = true;

  const bool is_drag_preview = is_tooltip && GetCapture() != NULL;

  bool is_splash = (!hwnd->m_parent && !hwnd->m_owner &&
                  !(hwnd->m_style & WS_CAPTION) && !(hwnd->m_style & WS_CHILD) &&
                  (!hwnd->m_title.Get() || !hwnd->m_title.Get()[0]));

  GtkWidget *gtk_win = gtk_window_new((is_popup_menu || is_tooltip) ? GTK_WINDOW_POPUP : GTK_WINDOW_TOPLEVEL);

  gtk_window_set_decorated(GTK_WINDOW(gtk_win), (hwnd->m_style & WS_CAPTION) != 0);
  const int scale = swell_gdk_window_scale(transient_for);

  gtk_widget_set_app_paintable(gtk_win, TRUE);
  g_signal_connect(gtk_win, "draw", G_CALLBACK(swell_wayland_draw), hwnd);
  gtk_widget_add_events(gtk_win, GDK_ALL_EVENTS_MASK|GDK_EXPOSURE_MASK);

  if (!is_popup_menu && !is_tooltip)
  {
    gtk_window_set_title(GTK_WINDOW(gtk_win), hwnd->m_title.Get());
  }

  gtk_window_set_default_size(GTK_WINDOW(gtk_win),
                               swell_gdk_ceil(r.right - r.left, scale) + (swell_wayland_needs_manual_titlebar(hwnd) ? SWELL_WAYLAND_BORDER_WIDTH*2 : 0),
                               swell_gdk_ceil(r.bottom - r.top, scale) + (swell_wayland_needs_manual_titlebar(hwnd) ? g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH*2 : 0));
  hwnd->m_oswidget = gtk_win;
  g_signal_connect(gtk_win, "notify::scale-factor", G_CALLBACK(swell_wayland_scale_changed), hwnd);

  if (g_swell_wayland_title_h)
  {
    gtk_style_context_add_class(gtk_widget_get_style_context(gtk_win), "swell");
    gtk_window_set_titlebar(GTK_WINDOW(gtk_win), gtk_label_new(NULL));
  }
  if (!is_popup_menu && !is_tooltip && !is_splash)
  {
    int rw = swell_gdk_ceil(r.right - r.left, scale), rh = swell_gdk_ceil(r.bottom - r.top, scale);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if (swell_wayland_needs_manual_titlebar(hwnd))
    {
      rw += SWELL_WAYLAND_BORDER_WIDTH*2;
      rh += g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH*2;
    }
    GdkGeometry gh;
    gh.min_width = gh.max_width = rw;
    gh.min_height = gh.max_height = rh;
    swell_wayland_set_geometry_hints(hwnd, &gh);
  }

  if (is_popup_menu && s_wayland_active_tooltip)
  {
    if (s_wayland_active_tooltip->m_oswindow)
      gdk_window_hide(s_wayland_active_tooltip->m_oswindow);
    s_wayland_active_tooltip = NULL;
  }
  if (is_tooltip && !is_drag_preview && PopupMenuIsActive())
  {
    gtk_widget_destroy(gtk_win);
    hwnd->m_oswidget = NULL;
    return false;
  }

  HWND popup_parent_hwnd = NULL;
  if (is_popup_menu || is_tooltip)
  {
    if (is_tooltip && !is_drag_preview) s_wayland_active_tooltip = hwnd;
    gtk_window_set_type_hint(GTK_WINDOW(gtk_win), is_tooltip ? GDK_WINDOW_TYPE_HINT_TOOLTIP : GDK_WINDOW_TYPE_HINT_POPUP_MENU);

    HWND parent_hwnd = NULL;

    if (is_popup_menu)
    {
      parent_hwnd = hwnd->m_owner;
      if (transient_for)
      {
        while (parent_hwnd && parent_hwnd->m_oswindow != transient_for)
          parent_hwnd = parent_hwnd->m_parent ? parent_hwnd->m_parent : parent_hwnd->m_owner;
      }
    }
    else
    {
      if (s_last_hover_oswindow)
      {
        HWND hov = swell_oswindow_to_hwnd(s_last_hover_oswindow);
        if (hov && hov != hwnd && hov->m_oswidget && !(hov->m_style & WS_CHILD))
          parent_hwnd = hov;
      }

      if (!parent_hwnd)
      {
        if (is_drag_preview)
        {
          parent_hwnd = GetFocus();
          while (parent_hwnd && !parent_hwnd->m_oswidget)
            parent_hwnd = parent_hwnd->m_parent;
        }
        else
        {
          s_wayland_active_tooltip = NULL;
          gtk_widget_destroy(gtk_win);
          hwnd->m_oswidget = NULL;
          return false;
        }
      }
    }

    if (parent_hwnd && parent_hwnd->m_oswidget)
      gtk_window_set_transient_for(GTK_WINDOW(gtk_win), GTK_WINDOW(parent_hwnd->m_oswidget));
    popup_parent_hwnd = parent_hwnd;
  }

  g_signal_connect(G_OBJECT(gtk_win), "enter-notify-event", G_CALLBACK(swell_wayland_handle_hover_enter), hwnd);
  g_signal_connect(G_OBJECT(gtk_win), "leave-notify-event", G_CALLBACK(swell_wayland_handle_hover_enter), hwnd);
  gtk_widget_realize(gtk_win);

  if ((is_popup_menu || (is_tooltip && !is_drag_preview)) && popup_parent_hwnd)
  {
    GdkWindow *pw = gtk_widget_get_window(gtk_win);
    GdkWindow *parent_gw = gtk_widget_get_window(popup_parent_hwnd->m_oswidget);
    if (pw && parent_gw)
    {
      const int psx = popup_parent_hwnd->m_position.left;
      const int psy = popup_parent_hwnd->m_position.top;
      const int parent_scale = swell_gdk_window_scale(parent_gw);
      GdkRectangle anchor = { swell_gdk_floor(r.left - psx, parent_scale), swell_gdk_floor(r.top - psy, parent_scale), 1, 1 };
      if (swell_wayland_needs_manual_titlebar(popup_parent_hwnd))
      {
        anchor.x += SWELL_WAYLAND_BORDER_WIDTH;
        anchor.y += g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH;
      }
      gdk_window_move_to_rect(pw, &anchor,
          GDK_GRAVITY_NORTH_WEST, GDK_GRAVITY_NORTH_WEST,
          (GdkAnchorHints)(GDK_ANCHOR_SLIDE_X | GDK_ANCHOR_SLIDE_Y),
          0, 0);
    }
  }
  hwnd->m_oswindow = gtk_widget_get_window(gtk_win);
  if (GDK_IS_WAYLAND_WINDOW(hwnd->m_oswindow) && !(hwnd->m_style & WS_CAPTION))
    gdk_wayland_window_announce_csd(hwnd->m_oswindow);
  return true;
}

static bool swell_wayland_is_maximized(HWND hwnd)
{
  if (hwnd && hwnd->m_oswidget && GTK_IS_WINDOW(hwnd->m_oswidget))
    return gtk_window_is_maximized(GTK_WINDOW(hwnd->m_oswidget));
  return hwnd && hwnd->m_is_maximized;
}

static void swell_wayland_paint_manual_titlebar(HWND hwnd, GdkWindow *window, cairo_t *cr)
{
  const int scale = swell_gdk_window_scale(window);
  const int w = swell_gdk_ceil(hwnd->m_position.right - hwnd->m_position.left, scale);
  if (w <= 0) return;
  const int bw = SWELL_WAYLAND_BORDER_WIDTH;
  const int full_w = w + bw*2;
  const int logical_h = swell_gdk_ceil(hwnd->m_position.bottom - hwnd->m_position.top, scale);
  const int full_h = g_swell_wayland_title_h + logical_h + bw*2;
  const bool is_focused = (hwnd->m_oswindow == SWELL_focused_oswindow);

  cairo_save(cr);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

  if (is_focused)
    cairo_set_source_rgb(cr, 60/255.0, 198/255.0, 251/255.0);
  else
    cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
  cairo_rectangle(cr, 0, 0, full_w, bw);
  cairo_fill(cr);
  cairo_rectangle(cr, 0, 0, bw, full_h);
  cairo_fill(cr);
  cairo_rectangle(cr, full_w - bw, 0, bw, full_h);
  cairo_fill(cr);
  cairo_rectangle(cr, 0, full_h - bw, full_w, bw);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
  cairo_rectangle(cr, bw, bw, full_w - bw*2, g_swell_wayland_title_h);
  cairo_fill(cr);

  const bool has_minmax = (hwnd->m_style & WS_THICKFRAME) != 0;
  const char *title = hwnd->m_title.Get();
  if (title && title[0])
  {
    cairo_save(cr);
    cairo_rectangle(cr, 8 + bw, bw,
      wdl_max(0, full_w - bw*2 - 16 - SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W * (has_minmax ? 3 : 1)),
      g_swell_wayland_title_h);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13.0);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, title, &ext);
    cairo_move_to(cr, 8 + bw, bw + (g_swell_wayland_title_h + ext.height) / 2.0);
    cairo_show_text(cr, title);
    cairo_restore(cr);
  }

  const double cy = bw + g_swell_wayland_title_h / 2.0;
  cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
  cairo_set_line_width(cr, 1.5);

  {
    const double cx = full_w - bw - SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W / 2.0;
    const double half = 5.0;
    cairo_move_to(cr, cx-half, cy-half); cairo_line_to(cr, cx+half, cy+half);
    cairo_move_to(cr, cx-half, cy+half); cairo_line_to(cr, cx+half, cy-half);
    cairo_stroke(cr);
  }

  if (has_minmax)
  {
    const double cx = full_w - bw - SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W * 1.5;
    const double half = 5.0;
    if (swell_wayland_is_maximized(hwnd))
    {
      cairo_rectangle(cr, cx-half+3, cy-half, half*2-3, half*2-3);
      cairo_stroke(cr);
      cairo_rectangle(cr, cx-half, cy-half+3, half*2-3, half*2-3);
      cairo_stroke(cr);
    }
    else
    {
      cairo_rectangle(cr, cx-half, cy-half, half*2, half*2);
      cairo_stroke(cr);
    }

    const double cx2 = full_w - bw - SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W * 2.5;
    cairo_move_to(cr, cx2-half, cy+half); cairo_line_to(cr, cx2+half, cy+half);
    cairo_stroke(cr);
  }

  cairo_restore(cr);
}

static int swell_wayland_titlebar_hittest(HWND hwnd, int x, int y)
{
  const int bw = SWELL_WAYLAND_BORDER_WIDTH;
  if (y < bw || y >= bw + g_swell_wayland_title_h) return 0;
  const int full_w = swell_gdk_ceil(hwnd->m_position.right - hwnd->m_position.left, swell_gdk_window_scale(hwnd->m_oswindow)) + bw*2;
  if (x >= full_w - bw - SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W) return 1;
  if (hwnd->m_style & WS_THICKFRAME)
  {
    if (x >= full_w - bw - SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W*2) return 3;
    if (x >= full_w - bw - SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W*3) return 4;
  }
  return 2;
}

#define SWELL_WAYLAND_RESIZE_MARGIN 6

static GdkWindowEdge swell_wayland_resize_edge_hittest(HWND hwnd, int x, int y, bool *found)
{
  *found = false;
  const int scale = swell_gdk_window_scale(hwnd->m_oswindow);
  int w = swell_gdk_ceil(hwnd->m_position.right - hwnd->m_position.left, scale);
  int h = swell_gdk_ceil(hwnd->m_position.bottom - hwnd->m_position.top, scale);
  if (swell_wayland_needs_manual_titlebar(hwnd))
  {
    w += SWELL_WAYLAND_BORDER_WIDTH*2;
    h += g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH*2;
  }
  const int m = SWELL_WAYLAND_RESIZE_MARGIN;

  const bool left = x < m, right = x >= w-m;
  const bool top = y < m, bottom = y >= h-m;

  if (!left && !right && !top && !bottom) return GDK_WINDOW_EDGE_SOUTH_EAST;
  *found = true;
  if (top && left) return GDK_WINDOW_EDGE_NORTH_WEST;
  if (top && right) return GDK_WINDOW_EDGE_NORTH_EAST;
  if (bottom && left) return GDK_WINDOW_EDGE_SOUTH_WEST;
  if (bottom && right) return GDK_WINDOW_EDGE_SOUTH_EAST;
  if (top) return GDK_WINDOW_EDGE_NORTH;
  if (bottom) return GDK_WINDOW_EDGE_SOUTH;
  if (left) return GDK_WINDOW_EDGE_WEST;
  return GDK_WINDOW_EDGE_EAST;
}

static void swell_wayland_motion_event(const GdkEventMotion *m)
{
  if (s_armed_drag_oswindow)
  {
    const int dx = (int)m->x_root - s_armed_drag_root_x;
    const int dy = (int)m->y_root - s_armed_drag_root_y;
    if (dx*dx + dy*dy >= 1*1)
    {
      gdk_window_begin_move_drag(s_armed_drag_oswindow, s_armed_drag_button,
                                  s_armed_drag_root_x, s_armed_drag_root_y, s_armed_drag_time);
      s_armed_drag_oswindow = NULL;
    }
  }
  {
    HWND top = swell_oswindow_to_hwnd(m->window);
    if (top && top->m_oswidget && !(top->m_style & WS_CHILD))
      s_last_hover_oswindow = m->window;

    static HWND s_last_edge_cursor_hwnd;
    static bool s_last_edge_cursor_set;
    if (top && (top->m_style & WS_THICKFRAME) && swell_wayland_needs_manual_titlebar(top) && top->m_oswindow)
    {
      bool found = false;
      GdkWindowEdge edge = swell_wayland_resize_edge_hittest(top, (int)m->x, (int)m->y, &found);
      if (found)
      {
        GdkCursorType ct;
        switch (edge)
        {
          case GDK_WINDOW_EDGE_NORTH_WEST: ct = GDK_TOP_LEFT_CORNER; break;
          case GDK_WINDOW_EDGE_NORTH: ct = GDK_TOP_SIDE; break;
          case GDK_WINDOW_EDGE_NORTH_EAST: ct = GDK_TOP_RIGHT_CORNER; break;
          case GDK_WINDOW_EDGE_WEST: ct = GDK_LEFT_SIDE; break;
          case GDK_WINDOW_EDGE_EAST: ct = GDK_RIGHT_SIDE; break;
          case GDK_WINDOW_EDGE_SOUTH_WEST: ct = GDK_BOTTOM_LEFT_CORNER; break;
          case GDK_WINDOW_EDGE_SOUTH: ct = GDK_BOTTOM_SIDE; break;
          default: ct = GDK_BOTTOM_RIGHT_CORNER; break;
        }
        GdkCursor *cursor = gdk_cursor_new_for_display(gdk_window_get_display(top->m_oswindow), ct);
        gdk_window_set_cursor(top->m_oswindow, cursor);
        g_object_unref(cursor);
        s_last_edge_cursor_hwnd = top;
        s_last_edge_cursor_set = true;
      }
      else if (s_last_edge_cursor_set && s_last_edge_cursor_hwnd == top)
      {
        gdk_window_set_cursor(top->m_oswindow, NULL);
        s_last_edge_cursor_set = false;
      }
    }
    else if (s_last_edge_cursor_set && top == s_last_edge_cursor_hwnd)
    {
      if (top->m_oswindow) gdk_window_set_cursor(top->m_oswindow, NULL);
      s_last_edge_cursor_set = false;
    }
  }
}

static bool swell_wayland_button_event(HWND hwnd, const GdkEventButton *b)
{
  if (b->type == GDK_BUTTON_RELEASE)
    s_armed_drag_oswindow = NULL;

  if (swell_wayland_needs_manual_titlebar(hwnd) && b->button == 1 &&
      (b->type == GDK_BUTTON_PRESS || b->type == GDK_2BUTTON_PRESS))
  {
    if (b->type == GDK_BUTTON_PRESS && (hwnd->m_style & WS_THICKFRAME) && hwnd->m_oswindow)
    {
      bool found = false;
      GdkWindowEdge edge = swell_wayland_resize_edge_hittest(hwnd, (int)b->x, (int)b->y, &found);
      if (found)
      {
        gdk_window_begin_resize_drag(hwnd->m_oswindow, edge, b->button, (gint)b->x_root, (gint)b->y_root, b->time);
        return true;
      }
    }

    const int hit = swell_wayland_titlebar_hittest(hwnd, (int)b->x, (int)b->y);
    if (b->type == GDK_2BUTTON_PRESS)
    {
      s_armed_drag_oswindow = NULL;
      if (hit == 2 && (hwnd->m_style & WS_THICKFRAME))
        swell_oswindow_maximize(hwnd, !swell_wayland_is_maximized(hwnd));
      if (hit) return true;
    }
    if (hit == 1)
    {
      swell_gdk_close(hwnd);
      return true;
    }
    else if (hit == 3)
    {
      swell_oswindow_maximize(hwnd, !swell_wayland_is_maximized(hwnd));
      return true;
    }
    else if (hit == 4)
    {
      if (hwnd->m_oswindow) gdk_window_iconify(hwnd->m_oswindow);
      return true;
    }
    else if (hit == 2)
    {
      if (hwnd->m_oswindow)
      {
        s_armed_drag_oswindow = hwnd->m_oswindow;
        s_armed_drag_button = b->button;
        s_armed_drag_root_x = (gint)b->x_root;
        s_armed_drag_root_y = (gint)b->y_root;
        s_armed_drag_time = b->time;
      }
      return true;
    }
  }
  return false;
}

static void swell_wayland_forget_window(HWND hwnd)
{
  if (s_wayland_active_tooltip == hwnd) s_wayland_active_tooltip = NULL;
  if (s_armed_drag_oswindow == hwnd->m_oswindow) s_armed_drag_oswindow = NULL;
  if (s_last_hover_oswindow && s_last_hover_oswindow == hwnd->m_oswindow)
    s_last_hover_oswindow = NULL;
  if (g_wayland_hovered_hwnd == hwnd) g_wayland_hovered_hwnd = NULL;
}

static void swell_wayland_set_geometry_hints(HWND hwnd, const GdkGeometry *hints)
{
  GObject *window = G_OBJECT(hwnd->m_oswidget);
  GdkGeometry *last = (GdkGeometry*)g_object_get_data(window, "swell-geometry-hints");
  if (last && last->min_width == hints->min_width && last->min_height == hints->min_height &&
      last->max_width == hints->max_width && last->max_height == hints->max_height) return;
  if (!last)
  {
    last = new GdkGeometry{};
    g_object_set_data_full(window, "swell-geometry-hints", last,
        [](gpointer data) { delete (GdkGeometry*)data; });
  }
  last->min_width = hints->min_width;
  last->min_height = hints->min_height;
  last->max_width = hints->max_width;
  last->max_height = hints->max_height;
  gtk_window_set_geometry_hints(GTK_WINDOW(window), NULL, last,
      (GdkWindowHints)(GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE));
}


static HWND s_wayland_drag_source, s_wayland_drag_destination;
static GtkWidget *s_wayland_drag_widget;
static POINT s_wayland_drag_position;
static bool s_wayland_drag_released, s_wayland_drag_cancelled;
static bool s_wayland_drag_outside_release;
static gboolean swell_wayland_drag_motion(GtkWidget *, GdkDragContext *, gint, gint, guint, gpointer);
static gboolean swell_wayland_drag_drop(GtkWidget *, GdkDragContext *, gint, gint, guint, gpointer);
static void swell_wayland_drag_leave(GtkWidget *, GdkDragContext *, guint, gpointer);

static void getMouseCoords(GdkWindow *window, double &x, double &y, double &x_root, double &y_root)
{
  HWND hwnd = swell_oswindow_to_hwnd(window);
  if (swell_wayland_needs_manual_titlebar(hwnd))
  {
    x -= SWELL_WAYLAND_BORDER_WIDTH;
    y -= g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH;
  }
  const int scale = swell_gdk_window_scale(window);
  x *= scale;
  y *= scale;
  x_root *= scale;
  y_root *= scale;

  if (hwnd && GDK_IS_WAYLAND_WINDOW(window))
  {
    x_root = x + hwnd->m_position.left;
    y_root = y + hwnd->m_position.top;
  }
}

static void swell_wayland_drag_set_destination(HWND hwnd)
{
  if (hwnd == s_wayland_drag_destination) return;
  if (hwnd) hwnd->Retain();
  if (s_wayland_drag_destination) s_wayland_drag_destination->Release();
  s_wayland_drag_destination = hwnd;
}

static bool swell_wayland_drag_target(POINT p, HWND *target)
{
  if (!s_wayland_drag_source) return false;
  *target = s_wayland_drag_destination;
  if (*target && !IsWindow(*target)) *target = NULL;
  if (*target)
  {
    p.x -= (*target)->m_position.left;
    p.y -= (*target)->m_position.top;
    *target = ChildWindowFromPoint(*target, p);
  }
  return true;
}

static void swell_wayland_drag_mouse(UINT msg)
{
  HWND hwnd = s_wayland_drag_source;
  if (!hwnd || GetCapture() != hwnd) return;
  POINT p = s_wayland_drag_position;
  ScreenToClient(hwnd, &p);
  SWELL_SendMouseMessage(hwnd, msg, 0, MAKELPARAM(p.x, p.y),
                         s_wayland_drag_position.x, s_wayland_drag_position.y);
}

static bool swell_wayland_drag_update(GtkWidget *widget, GdkDragContext *context,
                                      gint x, gint y, HWND hwnd)
{
  if (!s_wayland_drag_source || gtk_drag_get_source_widget(context) != s_wayland_drag_widget)
    return false;
  swell_wayland_drag_set_destination(hwnd);
  if (hwnd && hwnd->m_oswindow) s_last_hover_oswindow = hwnd->m_oswindow;
  double px = x, py = y, rx = 0, ry = 0;
  getMouseCoords(gtk_widget_get_window(widget), px, py, rx, ry);
  s_wayland_drag_position.x = (int)rx;
  s_wayland_drag_position.y = (int)ry;
  swell_lastMessagePos = MAKELONG((int)rx, (int)ry);
  return true;
}

static gboolean swell_wayland_drag_motion(GtkWidget *widget, GdkDragContext *context,
                                          gint x, gint y, guint time, gpointer data)
{
  if (!swell_wayland_drag_update(widget, context, x, y, (HWND)data)) return FALSE;
  gdk_drag_status(context, GDK_ACTION_COPY, time);
  swell_wayland_drag_mouse(WM_MOUSEMOVE);
  return TRUE;
}

static gboolean swell_wayland_drag_drop(GtkWidget *widget, GdkDragContext *context,
                                        gint x, gint y, guint time, gpointer data)
{
  if (!swell_wayland_drag_update(widget, context, x, y, (HWND)data)) return FALSE;
  s_wayland_drag_released = true;
  swell_wayland_drag_mouse(WM_LBUTTONUP);
  gtk_drag_finish(context, TRUE, FALSE, time);
  return TRUE;
}

static void swell_wayland_drag_leave(GtkWidget *, GdkDragContext *context, guint, gpointer data)
{
  if (!s_wayland_drag_source || s_wayland_drag_destination != (HWND)data ||
      gtk_drag_get_source_widget(context) != s_wayland_drag_widget) return;
  swell_wayland_drag_set_destination(NULL);
}

static gboolean swell_wayland_drag_failed(GtkWidget *, GdkDragContext *, GtkDragResult result, gpointer)
{
  if (!s_wayland_drag_source || s_wayland_drag_released || !s_wayland_drag_outside_release ||
      (result != GTK_DRAG_RESULT_NO_TARGET && result != GTK_DRAG_RESULT_ERROR)) return FALSE;
  swell_wayland_drag_set_destination(NULL);
  s_wayland_drag_released = true;
  swell_wayland_drag_mouse(WM_LBUTTONUP);
  return TRUE;
}

static void swell_wayland_drag_end(GtkWidget *widget, GdkDragContext *, gpointer)
{
  if (!s_wayland_drag_source) return;
  swell_wayland_drag_set_destination(NULL);
  s_wayland_drag_cancelled = !s_wayland_drag_released;
  s_wayland_drag_released = true;
  if (s_wayland_drag_cancelled) swell_wayland_drag_mouse(WM_MOUSEMOVE);
  if (GetCapture() == s_wayland_drag_source) ReleaseCapture();
  s_wayland_drag_source->Release();
  s_wayland_drag_source = NULL;
  s_wayland_drag_widget = NULL;
  g_signal_handlers_disconnect_by_data(widget, &s_wayland_drag_source);
  g_object_unref(widget);
}

static void swell_wayland_begin_drag(HWND source)
{
  HWND capture = GetCapture();
  if (s_wayland_drag_source || swell_gdk_drag_source || !capture || capture == source || !swell_gdk_current_event ||
      swell_gdk_current_event->type != GDK_MOTION_NOTIFY ||
      !(swell_gdk_current_event->motion.state & GDK_BUTTON1_MASK) ||
      !GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default())) return;
  GtkWidget *widget = gtk_get_event_widget(swell_gdk_current_event);
  if (!widget) return;
  widget = gtk_widget_get_toplevel(widget);
  GetCursorPos(&s_wayland_drag_position);
  capture->Retain();
  s_wayland_drag_source = capture;
  s_wayland_drag_widget = (GtkWidget*)g_object_ref(widget);
  s_wayland_drag_released = false;
  s_wayland_drag_cancelled = false;
  s_wayland_drag_outside_release = false;
  g_signal_connect(widget, "drag-end", G_CALLBACK(swell_wayland_drag_end), &s_wayland_drag_source);
  g_signal_connect(widget, "drag-failed", G_CALLBACK(swell_wayland_drag_failed), &s_wayland_drag_source);
  GtkTargetEntry target = { (gchar*)"application/x-swell-drag", GTK_TARGET_SAME_APP, 1 };
  GtkTargetList *targets = gtk_target_list_new(&target, 1);
  GdkDragContext *context = gtk_drag_begin_with_coordinates(widget, targets, GDK_ACTION_COPY,
                                                           1, swell_gdk_current_event, -1, -1);
  gtk_target_list_unref(targets);
  if (!context) swell_wayland_drag_end(widget, NULL, NULL);
}

static bool swell_wayland_is_docker_tab(HWND hwnd)
{
  return hwnd && hwnd->m_classname && !strcmp(hwnd->m_classname, "WDLTabCtrl") &&
         hwnd->m_parent && !strcmp(hwnd->m_parent->m_title.Get(), "REAPER_dock");
}

static bool swell_wayland_check_docker_drag(const GdkEventMotion *m)
{
  if (!(m->state & GDK_BUTTON1_MASK) || !swell_wayland_is_docker_tab(GetCapture())) return false;
  GdkWindow *top = gdk_window_get_toplevel(m->window);
  if (!top) return false;
  if (m->x >= 0 && m->y >= 0 &&
      m->x < gdk_window_get_width(top) && m->y < gdk_window_get_height(top)) return false;
  swell_wayland_begin_drag(NULL);
  if (!s_wayland_drag_source) return false;
  s_wayland_drag_outside_release = true;
  return true;
}

static void on_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *data, guint info, guint time, gpointer user_data)
{
  HWND target_hwnd = (HWND)user_data;

  gint len = gtk_selection_data_get_length(data);
  const guchar *gptr = gtk_selection_data_get_data(data);

  if (len > 0 && gptr)
  {
    const int scale = swell_gdk_widget_scale(widget);
    POINT pt = {x * scale, y * scale};
    if (swell_wayland_needs_manual_titlebar(target_hwnd))
    {
      pt.x -= SWELL_WAYLAND_BORDER_WIDTH * scale;
      pt.y -= (g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH) * scale;
    }

    HWND child = ChildWindowFromPoint(target_hwnd, pt);
    if (!child)
      child = target_hwnd;

    ClientToScreen(target_hwnd, &pt);
    ScreenToClient(child, &pt);

    HANDLE hdrop = swell_gdk_urilist_to_dropfiles(&pt, gptr, len);
    if (hdrop)
    {
      SendMessage(child, WM_DROPFILES, (WPARAM)hdrop, 0);
      GlobalFree(hdrop);
    }
    gtk_drag_finish(context, TRUE, FALSE, time);
  }
  else
  {
    gtk_drag_finish(context, FALSE, FALSE, time);
  }
}

static void swell_wayland_setup_drag(HWND hwnd)
{
  GtkWidget *gtk_win = hwnd->m_oswidget;
  if (gtk_win && gtk_window_get_window_type(GTK_WINDOW(gtk_win)) == GTK_WINDOW_TOPLEVEL)
  {
    gtk_drag_dest_set(gtk_win, (GtkDestDefaults)(GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP), NULL, 0, GDK_ACTION_COPY);
    GtkTargetEntry targets[] = {
      { (gchar*)"text/uri-list", 0, 0 },
      { (gchar*)"application/x-swell-drag", GTK_TARGET_SAME_APP, 1 }
    };
    GtkTargetList *list = gtk_target_list_new(targets, 2);
    gtk_drag_dest_set_target_list(gtk_win, list);
    gtk_target_list_unref(list);
    g_signal_connect(gtk_win, "drag-data-received", G_CALLBACK(on_drag_data_received), hwnd);
    g_signal_connect(gtk_win, "drag-motion", G_CALLBACK(swell_wayland_drag_motion), hwnd);
    g_signal_connect(gtk_win, "drag-drop", G_CALLBACK(swell_wayland_drag_drop), hwnd);
    g_signal_connect(gtk_win, "drag-leave", G_CALLBACK(swell_wayland_drag_leave), hwnd);
  }
}


static void mouse_event(GdkEvent *event)
{
  GdkEvent pixels = {};
  switch (event->type)
  {
    case GDK_MOTION_NOTIFY:
      if (s_wayland_drag_source || swell_wayland_check_docker_drag(&event->motion)) return;
      pixels.motion = event->motion;
      swell_wayland_motion_event(&pixels.motion);
      getMouseCoords(pixels.motion.window, pixels.motion.x, pixels.motion.y,
                     pixels.motion.x_root, pixels.motion.y_root);
      swell_gdk_motion_event(&pixels.motion);
      break;
    case GDK_SCROLL:
      pixels.scroll = event->scroll;
      getMouseCoords(pixels.scroll.window, pixels.scroll.x, pixels.scroll.y,
                     pixels.scroll.x_root, pixels.scroll.y_root);
      swell_gdk_scroll_event(&pixels.scroll);
      break;
    default:
      if (s_wayland_drag_source) return;
      HWND hwnd = swell_oswindow_to_hwnd(event->any.window);
      if (!hwnd) return;
      if (event->type == GDK_BUTTON_PRESS && xw_bridge_swell_on_button_event_escape()) return;
      if (swell_wayland_button_event(hwnd, &event->button)) return;
      pixels.button = event->button;
      getMouseCoords(pixels.button.window, pixels.button.x, pixels.button.y,
                     pixels.button.x_root, pixels.button.y_root);
      swell_gdk_button_event(&pixels.button);
      break;
  }
}

static void event_handler(GdkEvent *event)
{
  if (event->type == GDK_EXPOSE && xw_bridge_draw(event->any.window)) return;
  if (event->type == GDK_KEY_PRESS || event->type == GDK_KEY_RELEASE)
  {
    GdkEventKey *key = &event->key;
    if (xw_bridge_key_event(key)) return;
    if (!s_wayland_drag_source)
    {
      swell_gdk_prepare_key_event();
      HWND hwnd = swell_oswindow_to_hwnd(key->window);
      if (hwnd && !xw_bridge_raise_modals())
      {
        HWND focus = GetFocusIncludeMenus();
        if (focus && focus->m_classname && !strcmp(focus->m_classname, "REAPERXBridge"))
          xw_bridge_forward_key(focus, key->hardware_keycode, key->state, key->type == GDK_KEY_PRESS);
        else if (key->type != GDK_KEY_PRESS || !xw_bridge_swell_on_button_event_escape())
          swell_gdk_key_event(key);
      }
    }
  }
  if (event->type == GDK_GRAB_BROKEN && s_wayland_drag_source)
  {
    gtk_main_do_event(event);
    return;
  }
  if (event->type == GDK_EXPOSE) expose(&event->expose, NULL, true);
  else if (event->type == GDK_CONFIGURE) configure(&event->configure);
  swell_gdk_dispatch_event(event);
  if (event->type == GDK_EXPOSE && !gdk_window_is_destroyed(event->any.window) &&
      swell_oswindow_to_hwnd(event->any.window))
    gdk_window_set_background_pattern(event->any.window, NULL);
}

static void run_events()
{
  GMainContext *ctx=g_main_context_default();
  const gint64 deadline = g_get_monotonic_time() + 4000;
  guint wait_source = 0;
  for (;;)
  {
    bool dispatched = g_main_context_iteration(ctx,FALSE);
    if (!dispatched && xw_bridge_has_pending_draw() && g_get_monotonic_time() < deadline)
    {
      if (!wait_source)
        wait_source = g_timeout_add_full(GDK_PRIORITY_EVENTS,
          max(1, (int)((deadline - g_get_monotonic_time() + 999) / 1000)),
          [](gpointer) -> gboolean { return G_SOURCE_CONTINUE; }, NULL, NULL);
      dispatched = g_main_context_iteration(ctx,TRUE);
    }
    if (!dispatched) break;
    if (g_get_monotonic_time() >= deadline) break;
  }
  if (wait_source) g_source_remove(wait_source);
  gdk_display_flush(gdk_display_get_default());

}

static void get_cursor_pos(POINT *pt)
{
  if (s_wayland_drag_source)
  {
    *pt = s_wayland_drag_position;
    return;
  }
  GdkWindow *window = gdk_display_get_window_at_pointer(gdk_display_get_default(), NULL, NULL);
  if (window) window = gdk_window_get_toplevel(window);
  if (swell_oswindow_to_hwnd(window))
  {
    gint px = 0, py = 0;
    gdk_window_get_pointer(window, &px, &py, NULL);
    double x = px, y = py, x_root = 0, y_root = 0;
    getMouseCoords(window, x, y, x_root, y_root);
    pt->x = (int)x_root;
    pt->y = (int)y_root;
    return;
  }
  gdk_display_get_pointer(gdk_display_get_default(), NULL, &pt->x, &pt->y, NULL);
  window = swell_gdk_current_event ? swell_gdk_current_event->any.window : window;
  const int scale = swell_gdk_window_scale(window);
  pt->x *= scale;
  pt->y *= scale;
}

static WORD get_async_key_state(int key)
{
  if (s_wayland_drag_source && key == VK_LBUTTON)
    return s_wayland_drag_released ? 0 : 0x8000;
  if (s_wayland_drag_source && key == VK_ESCAPE && s_wayland_drag_cancelled)
    return 0x8000;
  return swell_gdk_get_async_key_state(key);
}

static void update_window(HWND hwnd, const RECT *rect)
{
  if (!hwnd || !hwnd->m_oswindow) return;
  GdkRectangle area = { rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top };
  area = swell_gdk_logical_rect(area, swell_gdk_window_scale(hwnd->m_oswindow));
  if (swell_wayland_needs_manual_titlebar(hwnd))
  {
    area.x += SWELL_WAYLAND_BORDER_WIDTH;
    area.y += g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH;
  }
  if (hwnd->m_oswidget)
    gtk_widget_queue_draw_area(hwnd->m_oswidget, area.x, area.y, area.width, area.height);
  else
    gdk_window_invalidate_rect(hwnd->m_oswindow, &area, FALSE);
}

static void invalidate(HWND hwnd, const RECT *r)
{
  GdkRectangle gdkr;
  if (r)
  {
    gdkr.x = r->left;
    gdkr.y = r->top;
    gdkr.width = r->right-r->left;
    gdkr.height = r->bottom-r->top;
    gdkr = swell_gdk_logical_rect(gdkr, swell_gdk_window_scale(hwnd->m_oswindow));
    if (swell_wayland_needs_manual_titlebar(hwnd))
    {
      gdkr.x += SWELL_WAYLAND_BORDER_WIDTH;
      gdkr.y += g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH;
    }
  }

  gdk_window_invalidate_rect(hwnd->m_oswindow,r ? &gdkr : NULL,true);
}

static void get_window_rect(HWND hwnd, RECT *r)
{
  *r = hwnd->m_position;
}

static void swell_wl_sizepin_release(GdkFrameClock *clock, gpointer data)
{
  HWND hwnd = (HWND)data;
  g_signal_handlers_disconnect_by_func(clock, (gpointer)swell_wl_sizepin_release, data);
  hwnd->m_oswindow_private &= ~PRIVATE_SIZEPIN;
  swell_recalcMinMaxInfo(hwnd);
}

static void swell_wayland_release_sizepin(HWND hwnd)
{
  if (hwnd && (hwnd->m_oswindow_private & PRIVATE_SIZEPIN))
  {
    hwnd->m_oswindow_private &= ~PRIVATE_SIZEPIN;
    if (hwnd->m_oswindow)
      g_signal_handlers_disconnect_by_func(gdk_window_get_frame_clock(hwnd->m_oswindow),
                                           (gpointer)swell_wl_sizepin_release, hwnd);
  }
}

static void pin_window_size(HWND hwnd, RECT f)
{
  const int scale = swell_gdk_window_scale(hwnd->m_oswindow);
  int w = wdl_max(swell_gdk_ceil(f.right - f.left, scale), 1);
  int h = wdl_max(swell_gdk_ceil(f.bottom - f.top, scale), 1);
  if (swell_wayland_needs_manual_titlebar(hwnd))
  {
    w += SWELL_WAYLAND_BORDER_WIDTH*2;
    h += g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH*2;
  }

  GdkGeometry gh;
  memset(&gh,0,sizeof(gh));
  gh.min_width = gh.max_width = w;
  gh.min_height = gh.max_height = h;
  swell_wayland_set_geometry_hints(hwnd, &gh);
  gdk_window_set_geometry_hints(hwnd->m_oswindow, &gh,
    (GdkWindowHints)(GDK_HINT_MIN_SIZE | GDK_HINT_MAX_SIZE));
  if (!(hwnd->m_oswindow_private & PRIVATE_SIZEPIN))
  {
    hwnd->m_oswindow_private |= PRIVATE_SIZEPIN;
    g_signal_connect(gdk_window_get_frame_clock(hwnd->m_oswindow), "after-paint",
                     G_CALLBACK(swell_wl_sizepin_release), hwnd);
  }
}

static void resize_window(GdkWindow *wnd, int reposflag, RECT f, bool mapped)
{
  HWND hwnd = swell_oswindow_to_hwnd(wnd);
  if (mapped && (reposflag & 2) && hwnd && hwnd->m_oswidget &&
      gtk_window_get_type_hint(GTK_WINDOW(hwnd->m_oswidget)) != GDK_WINDOW_TYPE_HINT_TOOLTIP)
  {
    pin_window_size(hwnd, f);
    return;
  }
  const int scale = swell_gdk_window_scale(wnd);
  const int width = swell_gdk_ceil(f.right - f.left, scale);
  const int height = swell_gdk_ceil(f.bottom - f.top, scale);
  f.left = swell_gdk_floor(f.left, scale);
  f.top = swell_gdk_floor(f.top, scale);
  f.right = f.left + width;
  f.bottom = f.top + height;
  if (swell_wayland_needs_manual_titlebar(hwnd))
  {
    const int w = f.right - f.left;
    const int h = f.bottom - f.top;
    f.left -= SWELL_WAYLAND_BORDER_WIDTH;
    f.top -= g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH;
    f.right = f.left + w + SWELL_WAYLAND_BORDER_WIDTH*2;
    f.bottom = f.top + h + g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH*2;
  }
  if ((reposflag&3)==3) gdk_window_move_resize(wnd,f.left,f.top,f.right-f.left,f.bottom-f.top);
  else if (reposflag&2) gdk_window_resize(wnd,f.right-f.left,f.bottom-f.top);
  else if (reposflag&1) gdk_window_move(wnd,f.left,f.top);
}

static void set_size_hints(HWND hwnd, const RECT *fixed)
{
  if (!fixed && (hwnd->m_oswindow_private & PRIVATE_SIZEPIN)) return;
  GdkGeometry h;
  if (!swell_gdk_size_hints(hwnd, fixed, &h)) return;
  const int scale = swell_gdk_window_scale(hwnd->m_oswindow);
  h.min_width = swell_gdk_ceil(h.min_width, scale);
  h.min_height = swell_gdk_ceil(h.min_height, scale);
  h.max_width = wdl_max(h.min_width, swell_gdk_floor(h.max_width, scale));
  h.max_height = wdl_max(h.min_height, swell_gdk_floor(h.max_height, scale));
  if (swell_wayland_needs_manual_titlebar(hwnd))
  {
    h.min_width += SWELL_WAYLAND_BORDER_WIDTH*2;
    h.max_width += SWELL_WAYLAND_BORDER_WIDTH*2;
    h.min_height += g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH*2;
    h.max_height += g_swell_wayland_title_h + SWELL_WAYLAND_BORDER_WIDTH*2;
  }
  swell_wayland_set_geometry_hints(hwnd, &h);
}

static void activate(bool) { }

static const char *window_manager_name()
{
  swell_wayland_init();
  return NULL;
}

static void show_window(HWND hwnd, bool, bool modal, guint32)
{
  if (modal) gtk_window_set_modal(GTK_WINDOW(hwnd->m_oswidget), TRUE);
  gdk_window_register_dnd(hwnd->m_oswindow);
  swell_wayland_setup_drag(hwnd);
  gtk_widget_show(hwnd->m_oswidget);
}

static void destroy_window(HWND hwnd)
{
  GtkWidget *widget = hwnd->m_oswidget;
  if (widget)
  {
    g_signal_handlers_disconnect_by_data(widget, hwnd);
    gtk_window_set_modal(GTK_WINDOW(widget), FALSE);
  }
  swell_wayland_release_sizepin(hwnd);
  if (swell_captured_window && hwnd->m_classname && !strcmp(hwnd->m_classname, "__SWELL_MENU"))
  {
    SendMessage(swell_captured_window, WM_CAPTURECHANGED, 0, 0);
    swell_captured_window = NULL;
  }
  swell_wayland_forget_window(hwnd);
  if (widget)
  {
    xw_bridge_detach_children(hwnd);
    if (widget == s_wayland_drag_widget)
    {
      g_signal_connect(widget, "drag-end", G_CALLBACK(+[](GtkWidget *w, GdkDragContext *, gpointer) {
        gtk_widget_destroy(w);
      }), NULL);
      gtk_widget_hide(widget);
    }
    else gtk_widget_destroy(widget);
  }
  else gdk_window_destroy(hwnd->m_oswindow);
  hwnd->m_oswidget = NULL;
}

static bool close_window(HWND hwnd)
{
  if (xw_bridge_popup_blocks_close(hwnd)) return false;
  xw_bridge_swell_on_gdk_delete_release(hwnd);
  return true;
}

static bool focus_window(HWND hwnd, bool, bool)
{
  gdk_window_raise(hwnd->m_oswindow);
  if (hwnd->m_oswindow == SWELL_focused_oswindow) return false;
  SWELL_focused_oswindow = hwnd->m_oswindow;
  gdk_window_focus(hwnd->m_oswindow, GDK_CURRENT_TIME);
  return true;
}

static void set_decorations(HWND hwnd)
{
  if (hwnd->m_oswidget)
    gtk_window_set_decorated(GTK_WINDOW(hwnd->m_oswidget), (hwnd->m_style & WS_CAPTION) != 0);
  if (GDK_IS_WAYLAND_WINDOW(hwnd->m_oswindow))
  {
    if (!(hwnd->m_style & WS_CAPTION) || g_swell_wayland_title_h)
      gdk_wayland_window_announce_csd(hwnd->m_oswindow);
    else
      gdk_wayland_window_announce_ssd(hwnd->m_oswindow);
  }
  swell_gdk_set_decorations(hwnd);
}

static void set_cursor(GdkWindow *window, GdkCursor *cursor, bool)
{
  gdk_window_set_cursor(window, cursor);
}

static HWND create_bridge(HWND parent, void **window, const RECT *rect)
{
  return xw_bridge_create(parent, window, rect, "REAPERXBridge");
}

static void invalidate_focus(GdkWindow *window)
{
  gdk_window_invalidate_rect(window, NULL, FALSE);
}

static bool get_viewport(RECT *r)
{
  GdkDisplay *disp = gdk_display_get_default();
  GdkMonitor *mon = SWELL_focused_oswindow ?
    gdk_display_get_monitor_at_window(disp, SWELL_focused_oswindow) : NULL;
  if (mon)
  {
    GdkRectangle rc = {0,};
    gdk_monitor_get_geometry(mon, &rc);
    r->left = 0;
    r->top = 0;
    const int scale = swell_gdk_window_scale(SWELL_focused_oswindow);
    r->right = rc.width * scale;
    r->bottom = rc.height * scale;
    return true;
  }
  return false;
}

static void monitor_geometry(int index, bool work, GdkRectangle *rect)
{
  swell_gdk_monitor_geometry(index, work, rect);
  const int scale = gdk_screen_get_monitor_scale_factor(gdk_screen_get_default(), index);
  rect->x *= scale;
  rect->y *= scale;
  rect->width *= scale;
  rect->height *= scale;
}

static void scaling_init(bool no_auto_hidpi)
{
  if (!no_auto_hidpi && g_swell_ui_scale == 256)
  {
    const int scale = swell_gdk_window_scale();
    if (scale > 1 && scale < 8) g_swell_ui_scale = scale * 256;
  }
}

static GtkClipboard *gtk_clipboard;
static bool clipboard_requested;
static guint clipboard_generation;
static void publish_wayland_clipboard();

static GtkClipboard *get_clipboard(void)
{
    if (!gtk_clipboard)
        gtk_clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    return gtk_clipboard;
}
static void clipboard_received_func(GtkClipboard *, GtkSelectionData *data, gpointer user_data)
{
  if (GPOINTER_TO_UINT(user_data) != clipboard_generation) return;
  clipboard_requested = false;
  if (!data) return;

  swell_gdk_clipboard.read_type = gtk_selection_data_get_target(data);
  const int len = gtk_selection_data_get_length(data);
  const guchar *bytes = gtk_selection_data_get_data(data);
  if (!bytes || len < 0) return;
  if (swell_gdk_clipboard.read_type == swell_gdk_urilist_atom())
    swell_gdk_clipboard.read_data = swell_gdk_urilist_to_dropfiles(NULL, bytes, len);
  else
  {
    swell_gdk_clipboard.read_data = GlobalAlloc(GMEM_MOVEABLE, len);
    if (swell_gdk_clipboard.read_data) memcpy(swell_gdk_clipboard.read_data, bytes, len);
  }
}

static void clipboard_received_text(GtkClipboard *, const gchar *text, gpointer user_data)
{
  if (GPOINTER_TO_UINT(user_data) != clipboard_generation) return;
  clipboard_requested = false;
  swell_gdk_clipboard.read_type = swell_gdk_utf8_atom();
  if (!text) return;
  WDL_FastString str;
  for (const char *p = text; *p; ++p)
  {
    if (*p == '\n' && (p == text || p[-1] != '\r')) str.Append("\r", 1);
    str.Append(p, 1);
  }
  const size_t len = str.GetLength() + 1;
  swell_gdk_clipboard.read_data = GlobalAlloc(GMEM_MOVEABLE, len);
  if (swell_gdk_clipboard.read_data) memcpy(swell_gdk_clipboard.read_data, str.Get(), len);
}

static HANDLE req_wayland_clipboard(GdkAtom type)
{
  if (swell_gdk_clipboard.read_type == type) return swell_gdk_clipboard.read_data;
  if (type == swell_gdk_targets_atom() && swell_gdk_clipboard.read_type == swell_gdk_atom_atom()) return swell_gdk_clipboard.read_data;

  GlobalFree(swell_gdk_clipboard.read_data);
  swell_gdk_clipboard.read_data = NULL;
  swell_gdk_clipboard.read_type = NULL;
  const guint request = ++clipboard_generation;
  clipboard_requested = true;
  if (type == swell_gdk_utf8_atom())
    gtk_clipboard_request_text(get_clipboard(), clipboard_received_text, GUINT_TO_POINTER(request));
  else
    gtk_clipboard_request_contents(get_clipboard(), type, clipboard_received_func, GUINT_TO_POINTER(request));

  const gint64 deadline = g_get_monotonic_time() + 500000;
  GMainContext *ctx = g_main_context_default();
  while (clipboard_generation == request && clipboard_requested && g_get_monotonic_time() < deadline)
    if (!g_main_context_iteration(ctx, FALSE)) g_usleep(1000);
  if (clipboard_generation != request) return NULL;
  if (clipboard_requested)
  {
    ++clipboard_generation;
    clipboard_requested = false;
    return NULL;
  }
  return swell_gdk_clipboard.read_data;
}

static void clipboard_get_func(GtkClipboard *, GtkSelectionData *data, guint, gpointer)
{
  GdkAtom target = gtk_selection_data_get_target(data);
  const bool text = gtk_targets_include_text(&target, 1);
  HANDLE *handle = swell_gdk_clipboard_find(text ? swell_gdk_utf8_atom() : target);
  if (!handle) return;
  WDL_FastString str;
  swell_gdk_clipboard_format(text ? swell_gdk_utf8_atom() : target, *handle, str);
  if (text) gtk_selection_data_set_text(data, str.Get(), str.GetLength());
  else gtk_selection_data_set(data, target, 8, (const guchar*)str.Get(), str.GetLength());
}

static void publish_wayland_clipboard()
{
  GtkTargetList *list = gtk_target_list_new(NULL, 0);
  for (int i = 0; i < swell_gdk_clipboard.formats.GetSize(); ++i)
  {
    GdkAtom target = swell_gdk_clipboard.formats.Get()[i];
    if (target == swell_gdk_utf8_atom()) gtk_target_list_add_text_targets(list, 0);
    else gtk_target_list_add(list, target, 0, 0);
  }
  gint count;
  GtkTargetEntry empty = {};
  GtkTargetEntry *targets = gtk_target_table_new_from_list(list, &count);
  gtk_clipboard_set_with_data(get_clipboard(), targets ? targets : &empty, count, clipboard_get_func, NULL, NULL);
  gtk_target_table_free(targets, count);
  gtk_target_list_unref(list);
}

static void clipboard_open()
{
  ++clipboard_generation;
  clipboard_requested = false;
}

static bool clipboard_text(GdkAtom atom)
{
  return gtk_targets_include_text(&atom, 1);
}

struct WaylandDragSource
{
  WaylandDragSource(const SWELL_GdkDrag &d) : data(d), state(0), dragctx(NULL), widget(NULL), finished(false) { }
  ~WaylandDragSource()
  {
    if (widget)
    {
      g_signal_handlers_disconnect_by_data(widget, this);
      if (dragctx) gtk_drag_cancel(dragctx);
      g_object_unref(widget);
    }
    if (dragctx) g_object_unref(dragctx);
  }
  const SWELL_GdkDrag &data;
  int state;
  GdkDragContext *dragctx;
  GtkWidget *widget;
  bool finished;
};

static void on_drag_source_data_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *data, guint info, guint time, gpointer user_data)
{
  WaylandDragSource *inf = (WaylandDragSource*)user_data;
  WDL_FastString str;

  if (inf->data.files && inf->data.count)
  {
    for (int x = 0; x < inf->data.count; x++)
    {
      str.Append("file://");
      swell_gdk_encode_uri(&str, inf->data.files[x]);
      str.Append("\r\n");
    }
  }
  else if (inf->data.callback && inf->data.filename)
  {
    if (!inf->state) inf->data.callback(inf->data.filename);
    inf->state = 1;
    str.Append("file://");
    swell_gdk_encode_uri(&str, inf->data.filename);
    str.Append("\r\n");
  }
  gtk_selection_data_set(data, swell_gdk_urilist_atom(), 8, (const guchar*)str.Get(), str.GetLength());
}

static void on_drag_source_end(GtkWidget *, GdkDragContext *, gpointer data)
{
  ((WaylandDragSource*)data)->finished = true;
  if (GetCapture() == swell_gdk_drag_source) ReleaseCapture();
}

static LRESULT WINAPI dropSourceWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  WaylandDragSource *inf = (WaylandDragSource*)hwnd->m_private_data;
  if (msg == WM_CREATE)
  {
    GtkWidget *widget = swell_gdk_current_event ? gtk_get_event_widget(swell_gdk_current_event) : NULL;
    if (!widget) return 0;
    inf->widget = (GtkWidget*)g_object_ref(gtk_widget_get_toplevel(widget));
    g_signal_connect(inf->widget, "drag-data-get", G_CALLBACK(on_drag_source_data_get), inf);
    g_signal_connect(inf->widget, "drag-end", G_CALLBACK(on_drag_source_end), inf);
    GtkTargetList *target_list = gtk_target_list_new(NULL, 0);
    gtk_target_list_add_uri_targets(target_list, 0);
    inf->dragctx = gtk_drag_begin_with_coordinates(inf->widget, target_list, GDK_ACTION_COPY, 1, swell_gdk_current_event, -1, -1);
    gtk_target_list_unref(target_list);
    if (inf->dragctx)
    {
      g_object_ref(inf->dragctx);
      if (!inf->finished) SetCapture(hwnd);
    }
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

static void drag_files(HWND hwnd, const SWELL_GdkDrag &data)
{
  WaylandDragSource info(data);
  RECT r = {};
  HWND h = new HWND__(NULL, 0, &r, NULL, false, NULL, dropSourceWndProc, NULL);
  swell_gdk_drag_source = h;
  h->m_private_data = (INT_PTR)&info;
  dropSourceWndProc(h, WM_CREATE, 0, 0);
  while (GetCapture() == h && (!hwnd || !hwnd->m_hashaddestroy))
  {
    SWELL_RunMessageLoop();
    Sleep(10);
  }
  swell_gdk_drag_source = NULL;
  DestroyWindow(h);
}

static HWND window_from_point(POINT p)
{
  HWND drag_target;
  if (swell_wayland_drag_target(p, &drag_target)) return drag_target;
  HWND hwnd = g_wayland_hovered_hwnd;
  if (hwnd)
  {
    HWND t = SWELL_topwindows;
    while (t && t != hwnd) t = t->m_next;
    hwnd = g_wayland_hovered_hwnd = t;
  }
  if (!hwnd) hwnd = swell_oswindow_to_hwnd(s_last_hover_oswindow);
  if (!hwnd || !PtInRect(&hwnd->m_position, p)) return NULL;
  p.x -= hwnd->m_position.left;
  p.y -= hwnd->m_position.top;
  return ChildWindowFromPoint(hwnd, p);
}

static void end_modal(HWND hwnd)
{
  if (hwnd->m_oswidget && GTK_IS_WINDOW(hwnd->m_oswidget))
    gtk_window_set_modal(GTK_WINDOW(hwnd->m_oswidget), FALSE);
}

const SWELL_GdkBackend swell_gdk_wayland = {
  event_handler, mouse_event, run_events, get_cursor_pos, get_async_key_state,
  update_window, invalidate, get_window_rect, resize_window, set_size_hints,
  activate, window_manager_name, swell_wayland_create_window, show_window, destroy_window,
  close_window, focus_window, set_decorations, set_cursor, create_bridge, invalidate_focus, true,
  get_viewport, monitor_geometry, scaling_init,
  clipboard_open, req_wayland_clipboard, NULL, publish_wayland_clipboard, clipboard_text, drag_files,
  swell_wayland_begin_drag, window_from_point, false, end_modal
};

#endif
