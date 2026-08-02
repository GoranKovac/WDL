#ifdef SWELL_TARGET_WAYLAND

#include <gdk/gdkwayland.h>
#include <X11/Xlib.h>
extern "C" {
#include <libwlembed/libwlembed.h>
#include <libwlembed-gtk3/libwlembed-gtk3.h>
}

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <signal.h>
#include <unistd.h>

static WleEmbeddedCompositor *s_wle_bridge_compositor;
static char s_wle_bridge_display[32];

#define SWELL_WAYLAND_BRIDGE_DISPLAY_NUM 20
#define SWELL_WAYLAND_BRIDGE_READY_TIMEOUT_MS 5000
#define SWELL_WAYLAND_BRIDGE_POLL_INTERVAL_MS 50

static bool swell_wayland_bridge_wait_for_display_ready(int display_num)
{
  char sockpath[64];
  snprintf(sockpath, sizeof(sockpath), "/tmp/.X11-unix/X%d", display_num);

  int waited_ms = 0;
  while (waited_ms < SWELL_WAYLAND_BRIDGE_READY_TIMEOUT_MS)
  {
    struct stat st;
    if (stat(sockpath, &st) == 0)
      return true;
    usleep(SWELL_WAYLAND_BRIDGE_POLL_INTERVAL_MS * 1000);
    waited_ms += SWELL_WAYLAND_BRIDGE_POLL_INTERVAL_MS;
  }
  return false;
}

const char *swell_wayland_bridge_get_display()
{
  return s_wle_bridge_display[0] ? s_wle_bridge_display : NULL;
}

bool swell_wayland_bridge_init()
{
  if (s_wle_bridge_compositor) return true;

  GdkDisplay *gdk_disp = gdk_display_get_default();
  if (!gdk_disp || !GDK_IS_WAYLAND_DISPLAY(gdk_disp))
    return false;

  GError *error = NULL;
  s_wle_bridge_compositor = wle_gtk_create_embedded_compositor("swell-wlembed-bridge-0", &error);
  if (!s_wle_bridge_compositor)
  {
    fprintf(stderr, "[swell] wayland bridge: failed to create embedded compositor: %s\n",
            error ? error->message : "(no error message)");
    if (error) g_error_free(error);
    return false;
  }

  wle_embedded_compositor_set_manage_child_processes(s_wle_bridge_compositor, TRUE);

  char cmdline[64];
  snprintf(cmdline, sizeof(cmdline), "xwayland-satellite :%d", SWELL_WAYLAND_BRIDGE_DISPLAY_NUM);

  if (!wle_embedded_compositor_spawn_command_line(s_wle_bridge_compositor, cmdline, &error))
  {
    fprintf(stderr, "[swell] wayland bridge: failed to spawn xwayland-satellite: %s\n",
            error ? error->message : "(no error message)");
    if (error) g_error_free(error);
    g_object_unref(s_wle_bridge_compositor);
    s_wle_bridge_compositor = NULL;
    return false;
  }

  if (!swell_wayland_bridge_wait_for_display_ready(SWELL_WAYLAND_BRIDGE_DISPLAY_NUM))
  {
    fprintf(stderr, "[swell] wayland bridge: timed out waiting for xwayland-satellite to become ready\n");
    g_object_unref(s_wle_bridge_compositor);
    s_wle_bridge_compositor = NULL;
    return false;
  }

  snprintf(s_wle_bridge_display, sizeof(s_wle_bridge_display), ":%d", SWELL_WAYLAND_BRIDGE_DISPLAY_NUM);
  setenv("DISPLAY", s_wle_bridge_display, 1);
  return true;
}

#include "swell.h"
#include "swell-internal.h"

struct XwBridgeWleState
{
  GtkWidget *socket;
  bool has_placed;
  Display *disp;
  Window container;
};

static void xw_bridge_wle_destroy(HWND hwnd)
{
  if (!hwnd || !hwnd->m_private_data) return;
  XwBridgeWleState *st = (XwBridgeWleState *)hwnd->m_private_data;

  fprintf(stderr, "[XWBRDBG] xw_bridge_wle_destroy start hwnd=%p widget=%p\n",
          (void*)hwnd, (void*)hwnd->m_oswidget);
  fflush(stderr);

  if (hwnd->m_oswidget && GTK_IS_WIDGET(hwnd->m_oswidget))
  {
    GtkWidget *widget = GTK_WIDGET(hwnd->m_oswidget);
    gtk_widget_hide(widget);

    g_object_remove_weak_pointer(G_OBJECT(widget), (gpointer *)&hwnd->m_oswidget);
    hwnd->m_oswidget = NULL;
  }

  if (st->disp)
  {
    st->disp = nullptr;
  }

  hwnd->m_private_data = 0;
  delete st;

  fflush(stderr);
}

static GtkWidget *xw_ensure_embed_widget(HWND h)
{
  if (!h) return NULL;

  if (h->m_oswidget)
  {
    GtkWidget *top = (GtkWidget *)h->m_oswidget;
    if (!GTK_IS_WINDOW(top)) return top;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(top));
    if (child) return child;
    GtkWidget *fixed = gtk_fixed_new();
    gtk_container_add(GTK_CONTAINER(top), fixed);
    gtk_widget_show(fixed);
    return fixed;
  }

  int w = h->m_position.right - h->m_position.left;
  int hh = h->m_position.bottom - h->m_position.top;
  if (w < 1) w = 1;
  if (hh < 1) hh = 1;

  GtkWidget *parent_widget = xw_ensure_embed_widget(h->m_parent);
  if (!parent_widget || !GTK_IS_FIXED(parent_widget)) return NULL;

  GtkWidget *existing = (GtkWidget *)GetProp(h, "XBridgeWleEmbedFixed");
  if (existing && GTK_IS_WIDGET(existing))
  {
    gtk_fixed_move(GTK_FIXED(parent_widget), existing, h->m_position.left, h->m_position.top);
    gtk_widget_set_size_request(existing, w, hh);
    return existing;
  }

  GtkWidget *fixed = gtk_fixed_new();
  gtk_widget_set_size_request(fixed, w, hh);
  gtk_fixed_put(GTK_FIXED(parent_widget), fixed, h->m_position.left, h->m_position.top);
  gtk_widget_show(fixed);

  SetProp(h, "XBridgeWleEmbedFixed", (HANDLE)fixed);
  return fixed;
}

static void xw_wle_size(HWND hwnd)
{
  if (!hwnd || !hwnd->m_private_data || !hwnd->m_oswidget) return;
  XwBridgeWleState *st = (XwBridgeWleState *)hwnd->m_private_data;

  GtkWidget *container = xw_ensure_embed_widget(hwnd->m_parent);
  if (!container || !GTK_IS_FIXED(container)) return;

  RECT r = hwnd->m_position;
  int pos_x = r.left, pos_y = r.top;
  int w = r.right - r.left, h = r.bottom - r.top;

  HWND top = hwnd->m_parent;
  while (top && !top->m_oswidget) top = top->m_parent;
  if (top && top->m_menu)
    pos_y += GetSystemMetrics(SM_CYMENU);

  fprintf(stderr, "[POSDBG] xw_wle_size hwnd=%p m_parent=%p container=%p pos=(%d,%d) size=(%d,%d)\n",
          (void*)hwnd, (void*)hwnd->m_parent, (void*)container, pos_x, pos_y, w, h);
  fflush(stderr);

  if (!st->has_placed)
  {
    gtk_fixed_put(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
    gtk_widget_set_size_request(hwnd->m_oswidget, w, h);
    gtk_widget_show(hwnd->m_oswidget);
    st->has_placed = true;
  }
  else
  {
    gtk_fixed_move(GTK_FIXED(container), hwnd->m_oswidget, pos_x, pos_y);
    gtk_widget_set_size_request(hwnd->m_oswidget, w, h);
  }
}

static LRESULT xw_bridge_wle_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  switch (msg)
  {
    case WM_MOVE:
    case WM_SIZE: xw_wle_size(hwnd); break;
    case WM_DESTROY: xw_bridge_wle_destroy(hwnd); break;
  }
  return 0;
}

HWND xw_bridge_create(HWND viewpar, void **wref, const RECT *r, const char *bridge_class_name)
{
  HWND hwnd = NULL;
  *wref = NULL;

  fprintf(stderr, "[POSDBG] xw_bridge_create viewpar=%p r=(%d,%d,%d,%d) viewpar->m_position=(%d,%d,%d,%d)\n",
          (void*)viewpar, r->left, r->top, r->right, r->bottom,
          viewpar ? viewpar->m_position.left : -1, viewpar ? viewpar->m_position.top : -1,
          viewpar ? viewpar->m_position.right : -1, viewpar ? viewpar->m_position.bottom : -1);
  fflush(stderr);

  const char *display_str = swell_wayland_bridge_get_display();

  Display *disp = XOpenDisplay(display_str);
  if (!disp)
  {
    hwnd = new HWND__(viewpar, 0, r, NULL, false, NULL);
    hwnd->m_classname = bridge_class_name;
    return hwnd;
  }

  int screen = DefaultScreen(disp);
  Window root = RootWindow(disp, screen);
  int w = wdl_max(r->right - r->left, 1);
  int h = wdl_max(r->bottom - r->top, 1);

  Window container = XCreateSimpleWindow(disp, root, 0, 0, w, h, 0,
                                         BlackPixel(disp, screen),
                                         WhitePixel(disp, screen));
  XMapWindow(disp, container);
  XFlush(disp);

  GtkWidget *socket = wle_gtk_socket_new(s_wle_bridge_compositor);
  const gchar *token = wle_gtk_socket_get_embedding_token(WLE_GTK_SOCKET(socket));
  fprintf(stderr, "[swell] wayland bridge: created socket, token=%s\n", token ? token : "(null)");

  hwnd = new HWND__(viewpar, 0, r, NULL, true, xw_bridge_wle_proc);
  hwnd->m_classname = bridge_class_name;
  hwnd->m_oswidget = socket;
  g_object_add_weak_pointer(G_OBJECT(socket), (gpointer *)&hwnd->m_oswidget);

  XwBridgeWleState *st = new XwBridgeWleState();
  st->socket = socket;
  st->has_placed = false;
  st->disp = disp;
  st->container = container;
  hwnd->m_private_data = (INT_PTR)st;

  xw_wle_size(hwnd);

  *wref = (void *)container;

  return hwnd;
}

#endif
