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


#if !defined(SWELL_PROVIDED_BY_APP) && defined(SWELL_TARGET_GDK)
#include <vector>
#include "swell.h"
#include "swell-internal.h"
#include <gdk/gdkx.h>
#include "swell-x11.h"
#include "swell-gdk.h"
#include <X11/extensions/XInput2.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xinput.h>
#include "swell-xcb.h"
#include <GL/glx.h>

static HWND s_ddrop_forward_last_hwnd; // last top level window, which receives events from the drag source
static Window s_ddrop_forward_last_target; // plug-in window we're sending XdndEnter etc to
static Window s_ddrop_forward_last_container; // bridged container window, we receive notifications back from s_ddrop_forward_last_target
static Window s_ddrop_forward_last_source; // external source of drag/drop, gets sent XdndStatus/XdndFinished, from s_ddrop_forward_last_hwnd->m_oswindow
static DWORD s_ddrop_forward_last_source_time; // last time source was known to be valid, treat as invalid after some small interval (10s?)
static bool s_ddrop_forward_last_has_dropped;

struct bridgeState {
  bridgeState(bool needrep, GdkWindow *_w, Window _nw, Display *_disp, GdkWindow *_curpar, HWND _hwnd_child);
  ~bridgeState();

  GdkWindow *w;
  Window native_w;
  Display *native_disp;
  xcb_connection_t *connection;
  GdkWindow *cur_parent;
  Window cur_parent_xid;
  Window cur_parent_xid2; // first child in xid, if any, otherwise cur_parent_xid
  HWND hwnd_child;

  bool lastvis;
  bool need_reparent;
  RECT lastrect;

  GLXContext gl_ctx;
};

static bridgeState *s_last_gl_ctx;

static WDL_PtrList<bridgeState> filter_windows;
bridgeState::~bridgeState()
{
  if (gl_ctx)
  {
    if (s_last_gl_ctx == this)
    {
      glXMakeCurrent(native_disp,None, NULL);
      s_last_gl_ctx = NULL;
    }

    glXDestroyContext(native_disp,gl_ctx);
    gl_ctx = NULL;
  }
  if (native_w && s_ddrop_forward_last_container == native_w)
    s_ddrop_forward_last_container = 0;
  filter_windows.DeletePtr(this);
  if (w)
  {
    if (!need_reparent)
    {
      // if this window is a child of another window, it will get destroyed by the hierarchy unless it is fully
      // released (we could do g_object_unref() again here, but reparenting feels safer in this context)
      gdk_window_reparent(w,NULL,0,0);
    }
    g_object_unref(G_OBJECT(w));
    xcb_discard_reply(connection, xcb_destroy_window_checked(connection, native_w).sequence);
  }
}

static xcb_window_t get_x11_first_child(xcb_connection_t *connection, xcb_window_t xid)
{
  if (!connection || !xid) return xid;
  xcb_query_tree_reply_t *tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, xid), NULL);
  xcb_window_t child = tree && xcb_query_tree_children_length(tree) ? xcb_query_tree_children(tree)[0] : xid;
  free(tree);
  return child;
}

bridgeState::bridgeState(bool needrep, GdkWindow *_w, Window _nw, Display *_disp, GdkWindow *_curpar, HWND _hwnd_child)
{
  hwnd_child = _hwnd_child;
  gl_ctx = NULL;
  w=_w;
  native_w=_nw;
  native_disp=_disp;
  connection=XGetXCBConnection(_disp);
  lastvis=false;
  need_reparent=needrep;
  cur_parent = _curpar;
  cur_parent_xid = _curpar ? GDK_WINDOW_XID(_curpar) : 0;
  cur_parent_xid2 = get_x11_first_child(connection, cur_parent_xid);
  memset(&lastrect,0,sizeof(lastrect));
  filter_windows.Add(this);
}

static LRESULT xbridgeProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
    case WM_DESTROY:
      if (hwnd && hwnd->m_private_data)
      {
        bridgeState *bs = (bridgeState*)hwnd->m_private_data;
        hwnd->m_private_data = 0;
        delete bs;
      }
    break;
    case WM_TIMER:

      if (wParam == 1010)
      {
        // callers can SetTimer(hwnd_container,1010,X,NULL) and have the child X window resized to fit the parent
        // they are only resized once, but it is deferred by timer until the window is actually there.
        // (if the window was created by another connection to the X server than ours, the window might not yet
        // be valid on the X server).
        bridgeState *bs = (bridgeState*)hwnd->m_private_data;

        RECT r;
        GetClientRect(hwnd,&r);
        if (r.right>0 && r.bottom>0 && bs)
        {
          // if a plug-in created a window on a separate X11 connection, it might not be valid yet.
          xcb_window_t child = get_x11_first_child(bs->connection, bs->native_w);
          if (child == bs->native_w) return 0;
          xcb_size_hints_t hints = {};
          if (xcb_icccm_get_wm_normal_hints_reply(bs->connection,
              xcb_icccm_get_wm_normal_hints(bs->connection, child), &hints, NULL))
          {
            if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
            {
              r.right = wdl_max(r.right, hints.min_width);
              r.bottom = wdl_max(r.bottom, hints.min_height);
            }
            if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)
            {
              if (hints.max_width > 0) r.right = wdl_min(r.right, hints.max_width);
              if (hints.max_height > 0) r.bottom = wdl_min(r.bottom, hints.max_height);
            }
          }
          const uint32_t size[] = { (uint32_t)r.right, (uint32_t)r.bottom };
          xcb_discard_reply(bs->connection, xcb_configure_window_checked(bs->connection, child,
              XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size).sequence);
          xcb_flush(bs->connection);
        }
        KillTimer(hwnd,wParam);
      }

      if (wParam != 1) break;
    case WM_MOVE:
    case WM_SIZE:
      if (hwnd && hwnd->m_private_data)
      {
        bridgeState *bs = (bridgeState*)hwnd->m_private_data;
        if (bs->w)
        {
          HWND h = hwnd->m_parent;
          RECT tr = hwnd->m_position;
          while (h)
          {
            RECT cr = h->m_position;
            if (h->m_oswindow)
            {
              cr.right -= cr.left;
              cr.bottom -= cr.top;
              cr.left=cr.top=0;
            }

            if (h->m_wndproc)
            {
              NCCALCSIZE_PARAMS p = {{ cr }};
              h->m_wndproc(h,WM_NCCALCSIZE,0,(LPARAM)&p);
              cr = p.rgrc[0];
            }
            tr.left += cr.left;
            tr.top += cr.top;
            tr.right += cr.left;
            tr.bottom += cr.top;

            if (tr.left < cr.left) tr.left=cr.left;
            if (tr.top < cr.top) tr.top = cr.top;
            if (tr.right > cr.right) tr.right = cr.right;
            if (tr.bottom > cr.bottom) tr.bottom = cr.bottom;

            if (h->m_oswindow) break;
            h=h->m_parent;
          }

          // todo: need to periodically check to see if the plug-in has resized its window
          bool vis = IsWindowVisible(hwnd);
          if (vis)
          {
#if SWELL_TARGET_GDK == 2
            gint w=0,hh=0,d=0;
            gdk_window_get_geometry(bs->w,NULL,NULL,&w,&hh,&d);
#else
            gint w=0,hh=0;
            gdk_window_get_geometry(bs->w,NULL,NULL,&w,&hh);
#endif
            if (w > bs->lastrect.right-bs->lastrect.left)
            {
              bs->lastrect.right = bs->lastrect.left + w;
              tr.right++; // workaround "bug" in GDK -- if bs->w was resized via Xlib, GDK won't resize it unless it thinks the size changed
            }
            if (hh > bs->lastrect.bottom-bs->lastrect.top)
            {
              bs->lastrect.bottom = bs->lastrect.top + hh;
              tr.bottom++; // workaround "bug" in GDK -- if bs->w was resized via Xlib, GDK won't resize it unless it thinks the size changed
            }
          }

          if (h && h->m_oswindow != bs->cur_parent)
            bs->need_reparent = true;

          if (h && (bs->need_reparent || (vis != bs->lastvis) || (vis&&memcmp(&tr,&bs->lastrect,sizeof(RECT)))))
          {
            if (bs->lastvis && !vis)
            {
              gdk_window_hide(bs->w);
              bs->lastvis = false;
            }

            if (bs->need_reparent)
            {
              gdk_window_reparent(bs->w,h->m_oswindow,tr.left,tr.top);
              gdk_window_resize(bs->w, tr.right-tr.left,tr.bottom-tr.top);
              bs->lastrect=tr;

              bs->cur_parent = h->m_oswindow;
              bs->cur_parent_xid = h->m_oswindow ? GDK_WINDOW_XID(h->m_oswindow) : 0;
              bs->cur_parent_xid2 = get_x11_first_child(bs->connection, bs->cur_parent_xid);
              bs->need_reparent=false;
              if (vis && bs->lastvis) gdk_window_show(bs->w);
            }
            else if (memcmp(&tr,&bs->lastrect,sizeof(RECT)))
            {
              bs->lastrect = tr;
              gdk_window_move_resize(bs->w,tr.left,tr.top, tr.right-tr.left, tr.bottom-tr.top);
            }
            if (vis && !bs->lastvis)
            {
              gdk_window_show(bs->w);
              gdk_window_raise(bs->w);
              bs->lastvis = true;
            }
          }
        }
      }
    break;
    case WM_USER+1000: // allow parent to query size of child window
      if (hwnd && hwnd->m_private_data && wParam && lParam)
      {
        bridgeState *bs = (bridgeState*)hwnd->m_private_data;
        if (bs->native_disp && bs->native_w)
        {
          // if a plug-in created a window on a separate X11 connection, it might not be valid yet.
          xcb_window_t child = get_x11_first_child(bs->connection, bs->native_w);
          if (child == bs->native_w) return 0;
          xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(bs->connection,
              xcb_get_geometry(bs->connection, child), NULL);
          if (geometry)
          {
            *((int *)(INT_PTR)wParam) = geometry->width;
            *((int *)(INT_PTR)lParam) = geometry->height;
            free(geometry);
          }
        }
      }
    break;
  }
  return DefWindowProc(hwnd,uMsg,wParam,lParam);
}


static void forward_x11_drag_message(int gdkmsg, GdkEventDND *gdkevent, Window new_target, Window container)
{
  GdkDisplay *display = gdk_window_get_display(gdkevent->window);
  xcb_connection_t *connection = XGetXCBConnection(gdk_x11_display_get_xdisplay(display));
  if (WDL_NOT_NORMALLY(!connection)) return;

  if (WDL_NOT_NORMALLY(!container)) return;

  xcb_client_message_event_t xev = {};
  xev.response_type = XCB_CLIENT_MESSAGE;
  xev.window = new_target;
  xev.format = 32;
  xev.data.data32[0] = container;

  switch (gdkmsg)
  {
    case GDK_DRAG_ENTER:
      {
        // make sure our containerwindow is tagged XdndAware, in case the plug-in checks
        const uint32_t ver = 5;
        xcb_change_property(connection, XCB_PROP_MODE_REPLACE, container,
            gdk_x11_get_xatom_by_name_for_display(display, "XdndAware"), XCB_ATOM_ATOM, 32, 1, &ver);
      }

      xev.type = gdk_x11_get_xatom_by_name_for_display(display, "XdndEnter");
      xev.data.data32[1] = (3 << 24); // protocol version 3
      {
        // mime types in 2/3/4, prioritizing any uri-list
        std::vector<xcb_atom_t> types;
        GList *targets = gdk_drag_context_list_targets(gdkevent->context);
        for (int pass = 0; pass < 2; ++pass)
          for (GList *l = targets; l; l = l->next)
          {
            gchar *name = gdk_atom_name((GdkAtom)l->data);
            if (name && (!strstr(name, "uri-list")) == pass)
              types.push_back(gdk_x11_atom_to_xatom_for_display(display, (GdkAtom)l->data));
            g_free(name);
          }
        for (size_t i = 0; i < types.size() && i < 3; ++i) xev.data.data32[2+i] = types[i];
        if (types.size() > 3)
        {
          xev.data.data32[1] |= 1;
          xcb_change_property(connection, XCB_PROP_MODE_REPLACE, container,
              gdk_x11_get_xatom_by_name_for_display(display, "XdndTypeList"),
              XCB_ATOM_ATOM, 32, types.size(), types.data());
        }
      }
    break;
    case GDK_DRAG_LEAVE:
      xev.type = gdk_x11_get_xatom_by_name_for_display(display, "XdndLeave");
    break;
    case GDK_DRAG_MOTION:
      xev.type = gdk_x11_get_xatom_by_name_for_display(display, "XdndPosition");
      xev.data.data32[1] = 0;
      xev.data.data32[2] = ((int)gdkevent->x_root << 16) | ((int)gdkevent->y_root & 0xFFFF);
      xev.data.data32[3] = gdkevent->time;
      switch (gdk_drag_context_get_suggested_action(gdkevent->context))
      {
        case GDK_ACTION_MOVE: xev.data.data32[4] = gdk_x11_get_xatom_by_name_for_display(display, "XdndActionMove"); break;
        case GDK_ACTION_LINK: xev.data.data32[4] = gdk_x11_get_xatom_by_name_for_display(display, "XdndActionLink"); break;
        case GDK_ACTION_PRIVATE: xev.data.data32[4] = gdk_x11_get_xatom_by_name_for_display(display, "XdndActionPrivate"); break;
        case GDK_ACTION_ASK: xev.data.data32[4] = gdk_x11_get_xatom_by_name_for_display(display, "XdndActionAsk"); break;
        default: xev.data.data32[4] = gdk_x11_get_xatom_by_name_for_display(display, "XdndActionCopy"); break;
      }
    break;
    case GDK_DROP_START:
      xev.type = gdk_x11_get_xatom_by_name_for_display(display, "XdndDrop");
      xev.data.data32[2] = gdkevent->time;
    break;
    default:
      WDL_ASSERT(false);
    return;
  }

  xcb_discard_reply(connection, xcb_send_event_checked(connection, false, new_target, 0, (const char*)&xev).sequence);
  xcb_flush(connection);
}


static bool validate_top_hwnd(HWND hwnd)
{
  if (!hwnd) return false;
  HWND h = SWELL_topwindows;
  while (h && h != hwnd) h = h->m_next;
  return h != NULL;
}

static bool validate_bridged_xw_from_tlhwnd(HWND hwnd, Window xw)
{
  if (WDL_NOT_NORMALLY(!hwnd || !hwnd->m_oswindow || !xw)) return false;

  for (int x = 0; x < filter_windows.GetSize(); x ++)
  {
    bridgeState *bs = filter_windows.Get(x);
    if (bs->cur_parent != hwnd->m_oswindow) continue;

    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(bs->connection,
        xcb_query_tree(bs->connection, bs->native_w), NULL);
    if (tree)
    {
      const xcb_window_t *children = xcb_query_tree_children(tree);
      bool found = false;
      for (int i = 0; i < xcb_query_tree_children_length(tree); ++i)
        if (children[i] == xw) { found = true; break; }
      free(tree);
      if (found) return true;
    }
  }
  return false;
}

static Window hit_test_bridged_xw(HWND hwnd, int xpos, int ypos, Window *containerOut)
{
  if (WDL_NOT_NORMALLY(!hwnd || !hwnd->m_oswindow)) return false;
  POINT pt = { xpos, ypos };
  GdkDisplay *display = gdk_window_get_display(hwnd->m_oswindow);
  Window new_target = 0;
  for (int x = 0; !new_target && x < filter_windows.GetSize(); x ++)
  {
    bridgeState *bs = filter_windows.Get(x);
    if (bs->cur_parent != hwnd->m_oswindow) continue;
    if (!IsWindowVisible(bs->hwnd_child)) continue;

    RECT r;
    GetWindowRect(bs->hwnd_child,&r);
    if (!PtInRect(&r,pt)) continue;

    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(bs->connection,
        xcb_query_tree(bs->connection, bs->native_w), NULL);
    if (!tree) continue;
    const xcb_window_t *children = xcb_query_tree_children(tree);
    for (int i = 0; !new_target && i < xcb_query_tree_children_length(tree); ++i)
    {
      int lx = 0, ly = 0;
      if (!swell_xcb_translate(bs->connection, tree->root, children[i], xpos, ypos, &lx, &ly)) continue;
      xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(bs->connection,
          xcb_get_geometry(bs->connection, children[i]), NULL);
      const bool inside = geometry && lx >= 0 && ly >= 0 && lx < geometry->width && ly < geometry->height;
      free(geometry);
      if (!inside) continue;

      // make sure this window can do XdndAware
      xcb_get_property_reply_t *aware = xcb_get_property_reply(bs->connection,
          xcb_get_property(bs->connection, false, children[i],
              gdk_x11_get_xatom_by_name_for_display(display, "XdndAware"), XCB_ATOM_ATOM, 0, 1), NULL);
      if (aware && aware->format == 32 && aware->value_len)
      {
        // this would be where to check for XdndProxy and resolve it.
        new_target = children[i];
        *containerOut = bs->native_w;
      }
      free(aware);
    }
    free(tree);
  }
  return new_target;
}


static void run_drag_finish_abort()
{
  GdkWindow *window = s_ddrop_forward_last_hwnd->m_oswindow;
  GdkDisplay *display = gdk_window_get_display(window);
  xcb_connection_t *connection = XGetXCBConnection(gdk_x11_display_get_xdisplay(display));
  xcb_client_message_event_t event = {};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.window = s_ddrop_forward_last_source;
  event.type = gdk_x11_get_xatom_by_name_for_display(display, "XdndFinished");
  event.format = 32;
  event.data.data32[0] = GDK_WINDOW_XID(window);
  xcb_discard_reply(connection, xcb_send_event_checked(connection, false,
      s_ddrop_forward_last_source, 0, (const char*)&event).sequence);
  xcb_flush(connection);
}

bool swell_x11_drag_event(GdkEvent *evt)
{
  // GDK sends drag events to the top level window
  // we need to check for bridged child windows and delegate the events to them

  // GDK_DRAG_ENTER -- position might not be available for these
  // GDK_DRAG_LEAVE

  // GDK_DRAG_MOTION
  // GDK_DROP_FINISHED
  // GDK_DROP_START

  HWND hwnd = swell_oswindow_to_hwnd(((GdkEventAny*)evt)->window);
  if (!hwnd) return false;
  GdkEventDND *e = (GdkEventDND *)evt;

  switch (evt->type)
  {
    case GDK_DRAG_LEAVE:
      if (s_ddrop_forward_last_hwnd && s_ddrop_forward_last_hwnd == hwnd)
      {
        if (s_ddrop_forward_last_target)
        {
          if (validate_bridged_xw_from_tlhwnd(s_ddrop_forward_last_hwnd,s_ddrop_forward_last_target))
            forward_x11_drag_message(GDK_DRAG_LEAVE, e, s_ddrop_forward_last_target, s_ddrop_forward_last_container);
        }
        else if (SWELL_DDrop_onDragLeave) SWELL_DDrop_onDragLeave();
        s_ddrop_forward_last_target = 0;
        s_ddrop_forward_last_container = s_ddrop_forward_last_source = 0;
        s_ddrop_forward_last_hwnd = NULL;
      }
    break;
    case GDK_DROP_FINISHED:
      // todo this handler probably needs revisiting
      if (s_ddrop_forward_last_target && validate_top_hwnd(s_ddrop_forward_last_hwnd))
      {
        if (validate_bridged_xw_from_tlhwnd(s_ddrop_forward_last_hwnd,s_ddrop_forward_last_target))
          forward_x11_drag_message(GDK_DRAG_LEAVE, e, s_ddrop_forward_last_target, s_ddrop_forward_last_container);
      }
      s_ddrop_forward_last_target = 0;
      s_ddrop_forward_last_container = s_ddrop_forward_last_source = 0;
      s_ddrop_forward_last_hwnd = NULL;
      if (SWELL_DDrop_onDragLeave) SWELL_DDrop_onDragLeave();
    break;
    case GDK_DRAG_ENTER:
      if (s_ddrop_forward_last_has_dropped &&
          s_ddrop_forward_last_source &&
          validate_top_hwnd(s_ddrop_forward_last_hwnd) &&
          s_ddrop_forward_last_hwnd->m_oswindow)
      {
#ifdef _DEBUG
        printf("swell-generic-gdk: sending XdndFinished in response to GDK_DRAG_ENTER after previous stale drag\n");
#endif
        run_drag_finish_abort();
      }
      s_ddrop_forward_last_has_dropped = false;

      if (s_ddrop_forward_last_hwnd != hwnd && validate_top_hwnd(s_ddrop_forward_last_hwnd))
      {
        if (s_ddrop_forward_last_target)
        {
          if (validate_bridged_xw_from_tlhwnd(s_ddrop_forward_last_hwnd,s_ddrop_forward_last_target))
            forward_x11_drag_message(GDK_DRAG_LEAVE, e, s_ddrop_forward_last_target, s_ddrop_forward_last_container);
        }
        else if (SWELL_DDrop_onDragLeave) SWELL_DDrop_onDragLeave();
      }
      s_ddrop_forward_last_hwnd = hwnd;
      s_ddrop_forward_last_container = s_ddrop_forward_last_source = 0;
      s_ddrop_forward_last_target = 0;
      // position info is not yet available, assume top level window will get it
      if (WDL_NORMALLY(hwnd) && WDL_NORMALLY(e->context))
      {
        gdk_drag_status(e->context,GDK_ACTION_COPY,e->time);
        swell_gdk_drag_enter((int)e->x_root, (int)e->y_root);
      }
    return true;
    case GDK_DRAG_MOTION:
      {
        s_ddrop_forward_last_has_dropped = false;

        Window newcontainer = 0;
        Window xw = hit_test_bridged_xw(hwnd, e->x_root, e->y_root, &newcontainer);
        if (xw)
        {
          if (!s_ddrop_forward_last_target)
          {
            if (SWELL_DDrop_onDragLeave) SWELL_DDrop_onDragLeave();
          }

          if (xw != s_ddrop_forward_last_target)
          {
            if (s_ddrop_forward_last_target && validate_top_hwnd(s_ddrop_forward_last_hwnd) && validate_bridged_xw_from_tlhwnd(s_ddrop_forward_last_hwnd,s_ddrop_forward_last_target))
              forward_x11_drag_message(GDK_DRAG_LEAVE, e, s_ddrop_forward_last_target, s_ddrop_forward_last_container);

            forward_x11_drag_message(GDK_DRAG_ENTER, e, xw, newcontainer);
          }
          s_ddrop_forward_last_target = xw;
          s_ddrop_forward_last_container = newcontainer;

          s_ddrop_forward_last_source = GDK_WINDOW_XID(gdk_drag_context_get_source_window(e->context));
          s_ddrop_forward_last_source_time = GetTickCount();
        }
        else
        {
          if (s_ddrop_forward_last_target)
          {
            if (validate_top_hwnd(s_ddrop_forward_last_hwnd) && validate_bridged_xw_from_tlhwnd(s_ddrop_forward_last_hwnd,s_ddrop_forward_last_target))
              forward_x11_drag_message(GDK_DRAG_LEAVE, e, s_ddrop_forward_last_target, s_ddrop_forward_last_container);
            s_ddrop_forward_last_target = 0;
            s_ddrop_forward_last_container = 0;
            s_ddrop_forward_last_source = 0;
            swell_gdk_drag_enter((int)e->x_root, (int)e->y_root);
          }
        }
        s_ddrop_forward_last_hwnd = hwnd;
        if (xw)
        {
          forward_x11_drag_message(GDK_DRAG_MOTION, e, xw, newcontainer);
        }
        else
        {
          gdk_drag_status(e->context,GDK_ACTION_COPY,e->time);
          if (SWELL_DDrop_onDragOver)
          {
            POINT pt = { (int)e->x_root, (int)e->y_root };
            SWELL_DDrop_onDragOver(pt);
          }
        }
      }
    break;
    case GDK_DROP_START:
      {
        if (hwnd && hwnd == s_ddrop_forward_last_hwnd && s_ddrop_forward_last_target)
        {
          if (WDL_NORMALLY(validate_bridged_xw_from_tlhwnd(hwnd,s_ddrop_forward_last_target)))
          {
            s_ddrop_forward_last_source_time = GetTickCount();
            s_ddrop_forward_last_has_dropped = true;
            forward_x11_drag_message(GDK_DROP_START, e, s_ddrop_forward_last_target, s_ddrop_forward_last_container);
            // do not clear forwarding state until a XdndFinished was sent, or timed out on motion
          }
        }
        else
        {
          swell_gdk_receive_drop(hwnd, e);
        }
        if (SWELL_DDrop_onDragLeave) SWELL_DDrop_onDragLeave();
      }
    break;
    default: return false;
  }
  return false;
}


static const char * const bridge_class_name = "__swell_xbridgewndclass";

static bool want_key_embed_redirect(Display *disp, Window scan_id, Window *new_dest, int keycode, int modstate)
{
  for (int x=0;x<filter_windows.GetSize(); x++)
  {
    bridgeState *bs = filter_windows.Get(x);
    if (bs && bs->cur_parent &&
        (bs->cur_parent_xid == scan_id || bs->cur_parent_xid2 == scan_id) &&
        bs->native_disp == disp)
    {
      HWND foc = GetFocus();
      if (foc &&
          foc->m_classname &&
          !strcmp(foc->m_classname,bridge_class_name) &&
          foc->m_private_data == (INT_PTR)bs
          )
      {
        void *p = GetProp(foc,"SWELL_XBRIDGE_KBHOOK_CHECK");
        if (p && SendMessage(GetParent(foc),(int)(INT_PTR)p,keycode,modstate))
          return false;

        xcb_window_t child = get_x11_first_child(bs->connection, bs->native_w);
        if (child != bs->native_w)
        {
          *new_dest = child;
          return true;
        }
      }
    }
  }
  return false;
}

static GdkFilterReturn filterCreateShowProc(GdkXEvent *xev, GdkEvent *event, gpointer data)
{
  XEvent *xevent = (XEvent *)xev;
  if (WDL_NOT_NORMALLY(!xev)) return GDK_FILTER_CONTINUE;

  switch (xevent->type)
  {
    case KeyPress:
    case KeyRelease:
      {
        // only used if gdk_disable_multidevice() was called prior to gdk_init_ (maybe some env var too?)
        Window dest;
        Display *disp = xevent->xany.display;
        if (!xevent->xany.send_event &&
            want_key_embed_redirect(disp,xevent->xkey.window, &dest, xevent->xkey.keycode, xevent->xkey.state))
        {
          const XKeyEvent &src = xevent->xkey;
          xcb_key_press_event_t k = {};
          k.response_type = src.type;
          k.event = dest;
          k.root = src.root;
          k.child = src.subwindow;
          k.time = src.time;
          k.event_x = src.x; k.event_y = src.y;
          k.root_x = src.x_root; k.root_y = src.y_root;
          k.state = src.state;
          k.detail = src.keycode;
          k.same_screen = src.same_screen;
          xcb_connection_t *connection = XGetXCBConnection(disp);
          xcb_discard_reply(connection, xcb_send_event_checked(connection, false, dest, 0, (const char*)&k).sequence);
          xcb_flush(connection);
          return GDK_FILTER_REMOVE;
        }
        else if (xevent->xany.send_event)
        {
          Window scan_id = xevent->xkey.window;
          for (int x=0;x<filter_windows.GetSize(); x++)
          {
            bridgeState *bs = filter_windows.Get(x);
            if (bs && bs->native_disp == disp && bs->cur_parent_xid && bs->native_w == scan_id)
            {
              // redirect to the parent window for processing
              xevent->xkey.window = bs->cur_parent_xid;
              return GDK_FILTER_CONTINUE;
            }
          }
        }
      }
    break;
    case FocusIn:
      {
        // only used if gdk_disable_multidevice() was called prior to gdk_init_ (maybe some env var too?)
        Display *disp = xevent->xany.display;
        Window scan_id = xevent->xfocus.window;
        for (int x=0;x<filter_windows.GetSize(); x++)
        {
          bridgeState *bs = filter_windows.Get(x);
          if (bs && bs->cur_parent &&
              (bs->cur_parent_xid == scan_id || bs->cur_parent_xid2 == scan_id) &&
              bs->native_disp == disp)
          {
            POINT pt;
            GetCursorPos(&pt);
            RECT r;
            GetWindowRect(bs->hwnd_child,&r);
            if (PtInRect(&r,pt))
            {
              SetFocus(bs->hwnd_child);
              return GDK_FILTER_REMOVE;
            }
          }
        }
      }
    break;
    case GenericEvent:
      // XInput2
      {
        const xcb_query_extension_reply_t *xi = xcb_get_extension_data(
            XGetXCBConnection(xevent->xany.display), &xcb_input_id);
        if (!xi || !xi->present || xevent->xcookie.extension != xi->major_opcode) break;
        XIDeviceEvent *xievent = (XIDeviceEvent*)xevent->xcookie.data;
        if (!xevent->xany.send_event &&
            xievent &&
            (xievent->evtype == XI_KeyPress || xievent->evtype == XI_KeyRelease))
        {
          Window dest;
          Display *disp = xevent->xany.display;
          if (want_key_embed_redirect(disp,xievent->event, &dest, xievent->detail, xievent->mods.effective))
          {
            xcb_key_press_event_t k = {};
            k.response_type = xievent->evtype == XI_KeyPress ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
            k.event = dest;
            k.root = xievent->root;
            k.child = xievent->child;
            k.time = xievent->time;
            k.event_x = xievent->event_x; k.event_y = xievent->event_y;
            k.root_x = xievent->root_x; k.root_y = xievent->root_y;
            k.state = xievent->mods.effective;
            k.detail = xievent->detail;
            k.same_screen = true;
            xcb_connection_t *connection = XGetXCBConnection(disp);
            xcb_discard_reply(connection, xcb_send_event_checked(connection, false, dest, 0, (const char*)&k).sequence);
            xcb_flush(connection);
            return GDK_FILTER_REMOVE;
          }
        }
        else if (xievent && xievent->evtype == XI_FocusIn)
        {
          Display *disp = xevent->xany.display;
          XIFocusInEvent *foc = (XIFocusInEvent *)xievent;
          Window scan_id = foc->event;
          for (int x=0;x<filter_windows.GetSize(); x++)
          {
            bridgeState *bs = filter_windows.Get(x);
            if (bs && bs->cur_parent &&
                (bs->cur_parent_xid == scan_id || bs->cur_parent_xid2 == scan_id) &&
                bs->native_disp == disp)
            {
              POINT pt = { (int) foc->root_x, (int) foc->root_y };
              RECT r;
              GetWindowRect(bs->hwnd_child,&r);
              if (PtInRect(&r,pt))
              {
                SetFocus(bs->hwnd_child);
                return GDK_FILTER_REMOVE;
              }
            }
          }
        }
      }
    break;
    case CreateNotify:
      {
        for (int x=0;x<filter_windows.GetSize(); x++)
        {
          bridgeState *bs = filter_windows.Get(x);
          if (bs && bs->native_w == xevent->xany.window && bs->native_disp == xevent->xany.display)
          {
            //gint w=0,hh=0;
            //gdk_window_get_geometry(bs->w,NULL,NULL,&w,&hh);
            xcb_discard_reply(bs->connection, xcb_map_window_checked(bs->connection, xevent->xcreatewindow.window).sequence);
            //XResizeWindow(bs->native_disp, xevent->xcreatewindow.window,w,hh);
            return GDK_FILTER_REMOVE;
          }
        }
      }
    break;
    case ClientMessage:
      if (s_ddrop_forward_last_container &&
          xevent->xany.window == s_ddrop_forward_last_container &&
          s_ddrop_forward_last_hwnd &&
          s_ddrop_forward_last_source &&
          (GetTickCount()-s_ddrop_forward_last_source_time) < 10000)
      {
        if (!validate_top_hwnd(s_ddrop_forward_last_hwnd) || !s_ddrop_forward_last_hwnd->m_oswindow)
        {
          s_ddrop_forward_last_hwnd = NULL;
          s_ddrop_forward_last_container = s_ddrop_forward_last_source = 0;
          s_ddrop_forward_last_target = 0;
          s_ddrop_forward_last_has_dropped = false;
          break;
        }
        GdkDisplay *display = gdk_window_get_display(s_ddrop_forward_last_hwnd->m_oswindow);
        xcb_connection_t *connection = XGetXCBConnection(xevent->xany.display);
        const bool fin = xevent->xclient.message_type == gdk_x11_get_xatom_by_name_for_display(display, "XdndFinished");
        bool eat = false;
        if (fin || xevent->xclient.message_type == gdk_x11_get_xatom_by_name_for_display(display, "XdndStatus"))
        {
          // these messages need to get passed back to the drag source with our top-level window as the target
          xcb_client_message_event_t reply = {};
          reply.response_type = XCB_CLIENT_MESSAGE;
          reply.window = s_ddrop_forward_last_source;
          reply.type = xevent->xclient.message_type;
          reply.format = 32;
          for (int i = 0; i < 5; ++i) reply.data.data32[i] = xevent->xclient.data.l[i];
          reply.data.data32[0] = GDK_WINDOW_XID(s_ddrop_forward_last_hwnd->m_oswindow);
          xcb_discard_reply(connection, xcb_send_event_checked(connection, false,
              s_ddrop_forward_last_source, 0, (const char*)&reply).sequence);
          xcb_flush(connection);
          eat = true;
        }
        if (fin)
        {
          // drag complete, clear relay container state
          s_ddrop_forward_last_target = 0;
          s_ddrop_forward_last_container = s_ddrop_forward_last_source = 0;
          s_ddrop_forward_last_hwnd = NULL;
        }
        if (eat) return GDK_FILTER_REMOVE;
      }
    break;
  }
  return GDK_FILTER_CONTINUE;
}

HWND swell_x11_create_bridge(HWND viewpar, void **wref, const RECT *r)
{
  HWND hwnd = NULL;
  *wref = NULL;
  GdkWindow *ospar = NULL;
  HWND hpar = viewpar;
  while (hpar)
  {
    ospar = hpar->m_oswindow;
    if (ospar) break;
    hpar = hpar->m_parent;
  }

  bool need_reparent=false;

  if (!ospar)
  {
    need_reparent = true;
    ospar = gdk_screen_get_root_window(gdk_screen_get_default());
  }

  Display *disp = gdk_x11_display_get_xdisplay(gdk_window_get_display(ospar));
  xcb_connection_t *connection = XGetXCBConnection(disp);
  xcb_window_t w = xcb_generate_id(connection);
  xcb_generic_error_t *error = xcb_request_check(connection,
      xcb_create_window_checked(connection, XCB_COPY_FROM_PARENT, w, GDK_WINDOW_XID(ospar), 0, 0,
          wdl_max(r->right-r->left,1), wdl_max(r->bottom-r->top,1), 0,
          XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, NULL));
  if (error) { free(error); w = XCB_NONE; }
  GdkWindow *gdkw = w ? gdk_x11_window_foreign_new_for_display(gdk_display_get_default(),w) : NULL;

  hwnd = new HWND__(viewpar,0,r,NULL, true, xbridgeProc);
  bridgeState *bs = gdkw ? new bridgeState(need_reparent,gdkw,w,disp, ospar, hwnd) : NULL;
  hwnd->m_classname = bridge_class_name;
  hwnd->m_private_data = (INT_PTR) bs;
  if (gdkw)
  {
    *wref = (void *)(uintptr_t)w;

    const uint32_t events = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
                            XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(connection, w, XCB_CW_EVENT_MASK, &events);
    xcb_flush(connection);

    static bool filt_add;
    if (!filt_add)
    {
      filt_add=true;
      gdk_window_add_filter(NULL, filterCreateShowProc, NULL);
    }
    SetTimer(hwnd,1,100,NULL);
    if (!need_reparent) SendMessage(hwnd,WM_SIZE,SIZE_RESTORED,0);
  }
  return hwnd;
}


void swell_x11_drag_check_timeout()
{
    DWORD dt;
    if (s_ddrop_forward_last_has_dropped &&
        s_ddrop_forward_last_source &&
        (dt = GetTickCount() - s_ddrop_forward_last_source_time) > 3000 && // after 3s, timeout
        dt < 10000 &&
        validate_top_hwnd(s_ddrop_forward_last_hwnd) &&
        s_ddrop_forward_last_hwnd->m_oswindow)
    {
#ifdef _DEBUG
      printf("swell-generic-gdk: sending XdndFinished after timeout from stale drag\n");
#endif
      run_drag_finish_abort();
      s_ddrop_forward_last_hwnd = NULL;
      s_ddrop_forward_last_container = s_ddrop_forward_last_source = 0;
      s_ddrop_forward_last_target = 0;
      s_ddrop_forward_last_has_dropped = false;
    }
}

void SWELL_SetViewGL(HWND h, char wantGL)
{
  // only works for X bridge windows
  if (h && h->m_classname == bridge_class_name && h->m_private_data)
  {
    bridgeState *bs = (bridgeState*)h->m_private_data;
    if (wantGL && !bs->gl_ctx)
    {
      static GLint att[] = { GLX_RGBA, None };
      XVisualInfo* vi = glXChooseVisual(bs->native_disp, 0, att);
      if (vi)
      {
        bs->gl_ctx = glXCreateContext(bs->native_disp,vi,NULL,1);
        XFree(vi);
      }
    }
    else if (!wantGL && bs->gl_ctx)
    {
      if (s_last_gl_ctx == bs)
      {
        glXMakeCurrent(bs->native_disp,None, NULL);
        s_last_gl_ctx = NULL;
      }

      glXDestroyContext(bs->native_disp,bs->gl_ctx);
      bs->gl_ctx = NULL;
    }
  }
}

bool SWELL_GetViewGL(HWND h)
{
  return h && h->m_classname == bridge_class_name && h->m_private_data &&
    ((bridgeState*)h->m_private_data)->gl_ctx != NULL;
}

bool SWELL_SetGLContextToView(HWND h)
{
  if (h)
  {
    if (h->m_classname == bridge_class_name && h->m_private_data)
    {
      bridgeState *bs = (bridgeState*)h->m_private_data;
      if (bs->gl_ctx)
      {
        if (!glXMakeCurrent(bs->native_disp, bs->native_w, bs->gl_ctx)) return false;
        s_last_gl_ctx = bs;
        return true;
      }
    }
    return false;
  }

  if (s_last_gl_ctx)
  {
    glXMakeCurrent(s_last_gl_ctx->native_disp,None, NULL);
    s_last_gl_ctx = NULL;
  }
  return true;
}

guint32 swell_x11_get_desktop(GdkWindow *window)
{
  if (!window || !gdk_x11_screen_supports_net_wm_hint(gdk_window_get_screen(window),
      gdk_atom_intern_static_string("_NET_WM_DESKTOP"))) return 0;
  xcb_connection_t *connection = XGetXCBConnection(GDK_WINDOW_XDISPLAY(window));
  xcb_get_property_reply_t *reply = xcb_get_property_reply(connection,
      xcb_get_property(connection, false, GDK_WINDOW_XID(window),
          gdk_x11_get_xatom_by_name_for_display(gdk_window_get_display(window), "_NET_WM_DESKTOP"),
          XCB_ATOM_CARDINAL, 0, 1), NULL);
  const guint32 desktop = reply && reply->format == 32 && reply->value_len ?
      *(uint32_t*)xcb_get_property_value(reply) : 0;
  free(reply);
  return desktop;
}

void swell_x11_move_to_desktop(GdkWindow *window, guint32 desktop)
{
  if (!window || !gdk_x11_screen_supports_net_wm_hint(gdk_window_get_screen(window),
      gdk_atom_intern_static_string("_NET_WM_DESKTOP"))) return;
  xcb_connection_t *connection = XGetXCBConnection(GDK_WINDOW_XDISPLAY(window));
  xcb_client_message_event_t event = {};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.window = GDK_WINDOW_XID(window);
  event.type = gdk_x11_get_xatom_by_name_for_display(gdk_window_get_display(window), "_NET_WM_DESKTOP");
  event.format = 32;
  event.data.data32[0] = desktop;
  event.data.data32[1] = 1;
  xcb_send_event(connection, false, GDK_WINDOW_XID(gdk_screen_get_root_window(gdk_window_get_screen(window))),
      XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char*)&event);
  xcb_flush(connection);
}

bool swell_x11_has_focus(GdkWindow *window)
{
  // verify we already have focus
  GdkDisplay *display = gdk_window_get_display(window);
  xcb_connection_t *connection = XGetXCBConnection(gdk_x11_display_get_xdisplay(display));
  xcb_get_input_focus_reply_t *focus = xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), NULL);
  xcb_window_t cf = focus ? focus->focus : XCB_NONE;
  free(focus);
  if (!cf || (cf != GDK_WINDOW_XID(window) &&
              cf != get_x11_first_child(connection,GDK_WINDOW_XID(window))))
  {
#ifdef _DEBUG
    printf("swell-generic-gdk: our window (%d) was marked as focused, however another window (%d) was reported focused via GetInputFocus, correcting.\n",
        (int)GDK_WINDOW_XID(window), (int)cf);
#endif
    // some other window actually has focus, we missed a notification, apparently, correct this
    return false;
  }
  return true;
}

void swell_x11_force_focus(GdkWindow *window)
{
  xcb_connection_t *connection = XGetXCBConnection(GDK_WINDOW_XDISPLAY(window));
  xcb_discard_reply(connection, xcb_set_input_focus_checked(connection,
      XCB_INPUT_FOCUS_NONE, GDK_WINDOW_XID(window), XCB_CURRENT_TIME).sequence);
  xcb_flush(connection);
}

void swell_x11_set_grab_cursor(GdkWindow *window, GdkCursor *curs)
{
  // workaround for a GDK behavior:
  // gdkwindow.c, gdk_window_set_cursor_internal() has a line:
  // >>> if (_gdk_window_event_parent_of (window, pointer_info->window_under_pointer))
  // this should also allow setting the cursor if window is in a "grabbing" state
  GdkDisplay *gdkdisp = gdk_display_get_default();
#if SWELL_TARGET_GDK == 2
  if (gdkdisp && gdk_display_get_window_at_pointer(gdkdisp,NULL,NULL) != window)
#else
  GdkDevice *dev = gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdkdisp));
  if (dev && gdk_device_get_window_at_position(dev,NULL,NULL) != window)
#endif
  {
    Display *disp = gdk_x11_display_get_xdisplay(gdkdisp);
    Window wn =  GDK_WINDOW_XID(window);
#if SWELL_TARGET_GDK == 2
    gint devid=2; // hardcoded default pointing device
#else
    gint devid = gdk_x11_device_get_id(dev);
#endif
    if (disp && wn)
    {
      xcb_connection_t *connection = XGetXCBConnection(disp);
      xcb_discard_reply(connection, xcb_input_xi_change_cursor_checked(connection,
          wn, curs ? gdk_x11_cursor_get_xcursor(curs) : XCB_NONE, devid).sequence);
      xcb_flush(connection);
    }
  }
}

bool swell_x11_has_selection_owner(GdkDisplay *display, GdkAtom selection)
{
  xcb_connection_t *connection = XGetXCBConnection(gdk_x11_display_get_xdisplay(display));
  xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(connection,
      xcb_get_selection_owner(connection, gdk_x11_atom_to_xatom_for_display(display, selection)), NULL);
  const bool owned = reply && reply->owner != XCB_NONE;
  free(reply);
  return owned;
}

#endif
