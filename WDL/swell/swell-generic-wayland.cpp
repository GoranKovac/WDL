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
#include "xwayland-bridge.h"

HWND g_wayland_hovered_hwnd = NULL;

gboolean swell_wayland_handle_hover_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
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

int g_swell_wayland_title_h; // 0 = compositor has real SSD, use it normally
#define SWELL_WAYLAND_TITLEBAR_CLOSEBTN_W 36
int SWELL_WAYLAND_BORDER_WIDTH = 2;

bool swell_is_wayland()
{
  return GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
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
    "decoration, decoration:backdrop {"
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

void swell_wayland_init()
{
  swell_wayland_init_application_id();
  swell_wayland_init_titlebar_height();
  swell_wayland_suppress_csd_shadow_once();
}

static void swell_wayland_scale_changed(GtkWidget *widget, GParamSpec *, gpointer data)
{
  HWND hwnd = (HWND)data;
  if (!hwnd->m_oswindow || !gtk_widget_get_mapped(widget)) return;
  GdkEventConfigure cfg = {};
  cfg.window = hwnd->m_oswindow;
  gdk_window_get_geometry(cfg.window, &cfg.x, &cfg.y, &cfg.width, &cfg.height);
  hwnd->Retain();
  swell_gdk_configure(&cfg);
  if (!hwnd->m_hashaddestroy) InvalidateRect(hwnd, NULL, FALSE);
  hwnd->Release();
}

// Wayland allows only one popup per parent. Tooltip and a
// menu cannot both be showed at the same time.
// Track the single active tooltip
// so we can hide it when a menu opens, and suppress tooltips while a menu is up.
static HWND s_wayland_active_tooltip;
// Last real toplevel the pointer was over, recorded in OnMotionEvent. Tooltips have
// no parent/owner, so this is what tells us which window a tooltip belongs to.
// Stored as the OS window (re-resolved on use) so a destroyed window can't dangle.
SWELL_OSWINDOW s_last_hover_oswindow;
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
  if (gdk_cairo_get_clip_rectangle(cr, &exp.area)) swell_gdk_expose(&exp, cr);
  cairo_restore(cr);
  return FALSE;
}

bool swell_wayland_create_window(HWND hwnd, GdkWindow *transient_for)
{
  const RECT r = hwnd->m_position;
  bool is_popup_menu = (hwnd->m_classname && strcmp(hwnd->m_classname, "__SWELL_MENU") == 0);
  bool is_tooltip = (!hwnd->m_parent && !hwnd->m_owner && (hwnd->m_style & WS_CHILD) && (!hwnd->m_title.Get() || !hwnd->m_title.Get()[0]));

  // Imgui fix
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
    gtk_window_set_titlebar(GTK_WINDOW(gtk_win), gtk_label_new(NULL));
  }
  // GTK sizes a toplevel to its content's natural size and asks
  // the compositor for that — ignoring gtk_window_set_default_size when the
  // content is larger. That makes dialogs open full-height. A max-size hint
  // is the only thing GTK won't override, so clamp max to the requested size.
  if (!is_popup_menu && !is_tooltip && !is_splash)
  {
    // GTK ignores set_default_size when content natural size differs, and a
    // max-only hint just caps (doesn't force). To make the restored size
    // actually apply on Wayland, pin min=max=restored size — GTK cannot
    // override an exact min==max constraint. This is released on the FIRST
    // configure event (see swell_gdk_configure), i.e. once the window has been
    // mapped at this exact size, after which it is freely resizable.
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

  // Tooltip and a menu can't both be showed at same time,
  // opening a menu closes any active tooltip,
  // while a menu is open tooltips cant spawn.
  if (is_popup_menu && s_wayland_active_tooltip)
  {
    if (s_wayland_active_tooltip->m_oswindow)
      gdk_window_hide(s_wayland_active_tooltip->m_oswindow);
    s_wayland_active_tooltip = NULL;
  }
  if (is_tooltip && !is_drag_preview && PopupMenuIsActive())
  {
    // don't show a tooltip while a menu is up
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
      // Anchor to the window that was hovered, not the focused one.
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
          // The preview must always be visible, and its parent is only a
          // coordinate base (it is placed by gdk_window_move, not by an anchor
          // rect), so an imperfect parent is harmless here.
          parent_hwnd = GetFocus();
          while (parent_hwnd && !parent_hwnd->m_oswidget)
            parent_hwnd = parent_hwnd->m_parent;
        }
        else
        {
          // Fail closed: without a known hovered window we cannot place the
          // tooltip correctly, and showing it in the wrong window is worse than
          // not showing it at all.
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

  // gtk_window_move is not constrained to the screen,
  // so popups render outside of the bottom/right edge. Use gdk_window_move_to_rect
  // so the compositor slides the popup on-screen.
  // https://bugzilla.mozilla.org/show_bug.cgi?id=1423598
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

void swell_wayland_paint_manual_titlebar(HWND hwnd, GdkWindow *window, cairo_t *cr)
{
  const int scale = swell_gdk_window_scale(window);
  const int w = swell_gdk_ceil(hwnd->m_position.right - hwnd->m_position.left, scale);
  if (w <= 0) return;
  const int bw = SWELL_WAYLAND_BORDER_WIDTH;
  const int full_w = w + bw*2; // actual OS window width, including left+right border
  const int logical_h = swell_gdk_ceil(hwnd->m_position.bottom - hwnd->m_position.top, scale);
  const int full_h = g_swell_wayland_title_h + logical_h + bw*2; // top border + titlebar + content + bottom border
  const bool is_focused = (hwnd->m_oswindow == SWELL_focused_oswindow);

  cairo_save(cr);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);

  if (is_focused)
    cairo_set_source_rgb(cr, 60/255.0, 198/255.0, 251/255.0); // #3CC6FB
  else
    cairo_set_source_rgb(cr, 0.16, 0.16, 0.16);
  cairo_rectangle(cr, 0, 0, full_w, bw); // top
  cairo_fill(cr);
  cairo_rectangle(cr, 0, 0, bw, full_h); // left
  cairo_fill(cr);
  cairo_rectangle(cr, full_w - bw, 0, bw, full_h); // right
  cairo_fill(cr);
  cairo_rectangle(cr, 0, full_h - bw, full_w, bw); // bottom
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

  if (!left && !right && !top && !bottom) return GDK_WINDOW_EDGE_SOUTH_EAST; // unused, *found stays false
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

void swell_wayland_motion_event(const GdkEventMotion *m)
{
  if (s_armed_drag_oswindow)
  {
    const int dx = (int)m->x_root - s_armed_drag_root_x;
    const int dy = (int)m->y_root - s_armed_drag_root_y;
    if (dx*dx + dy*dy >= 1*1) // ~1px threshold before committing to an actual drag
    {
      gdk_window_begin_move_drag(s_armed_drag_oswindow, s_armed_drag_button,
                                  s_armed_drag_root_x, s_armed_drag_root_y, s_armed_drag_time);
      s_armed_drag_oswindow = NULL;
    }
  }
  // Remember which real toplevel the pointer is over. m->window is the OS window the
  // motion occurred on, so this is authoritative without needing global coordinates
  // (which Wayland does not provide). Skip popups: tooltips and menus are both
  // WS_CHILD, and letting the pointer entering a tooltip overwrite this would
  // re-anchor the next tooltip to the previous one.
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

bool swell_wayland_button_event(HWND hwnd, const GdkEventButton *b)
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

void swell_wayland_forget_window(HWND hwnd)
{
  if (s_wayland_active_tooltip == hwnd) s_wayland_active_tooltip = NULL;
  if (s_armed_drag_oswindow == hwnd->m_oswindow) s_armed_drag_oswindow = NULL;
  if (s_last_hover_oswindow && s_last_hover_oswindow == hwnd->m_oswindow)
    s_last_hover_oswindow = NULL;
  if (g_wayland_hovered_hwnd == hwnd) g_wayland_hovered_hwnd = NULL;
}

void swell_wayland_set_geometry_hints(HWND hwnd, const GdkGeometry *hints)
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

#endif
