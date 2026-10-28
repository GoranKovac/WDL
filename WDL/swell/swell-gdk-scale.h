#ifndef _SWELL_GDK_SCALE_H_
#define _SWELL_GDK_SCALE_H_

#include <gdk/gdkwayland.h>

static int swell_gdk_window_scale(GdkWindow *window = NULL)
{
  GdkDisplay *display = window ? gdk_window_get_display(window) : gdk_display_get_default();
  if (!display || !GDK_IS_WAYLAND_DISPLAY(display)) return 1;
  if (window) return gdk_window_get_scale_factor(window);

  GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
  if (!monitor && gdk_display_get_n_monitors(display)) monitor = gdk_display_get_monitor(display, 0);
  return monitor ? gdk_monitor_get_scale_factor(monitor) : 1;
}

static int swell_gdk_widget_scale(GtkWidget *widget)
{
  for (GtkWidget *w = widget; w; w = gtk_widget_get_parent(w))
    if (gtk_widget_get_window(w)) return swell_gdk_window_scale(gtk_widget_get_window(w));
  return swell_gdk_window_scale();
}

static int swell_gdk_floor(int pixels, int scale)
{
  return pixels / scale - (pixels % scale < 0);
}

static int swell_gdk_ceil(int pixels, int scale)
{
  return pixels / scale + (pixels % scale > 0);
}

static GdkRectangle swell_gdk_logical_rect(const GdkRectangle &pixels, int scale)
{
  GdkRectangle r;
  r.x = swell_gdk_floor(pixels.x, scale);
  r.y = swell_gdk_floor(pixels.y, scale);
  r.width = swell_gdk_ceil(pixels.x + pixels.width, scale) - r.x;
  r.height = swell_gdk_ceil(pixels.y + pixels.height, scale) - r.y;
  return r;
}

static void swell_gdk_queue_draw_pixels(GtkWidget *widget, int x, int y, int w, int h)
{
  GdkRectangle pixels = { x, y, w, h };
  GdkRectangle r = swell_gdk_logical_rect(pixels, swell_gdk_widget_scale(widget));
  gtk_widget_queue_draw_area(widget, r.x, r.y, r.width, r.height);
}

#endif
