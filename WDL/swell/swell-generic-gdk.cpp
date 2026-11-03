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


#ifndef SWELL_PROVIDED_BY_APP
#include "swell.h"


#ifdef SWELL_PRELOAD
#define STR(x) #x
#define STR2(x) STR(x)
extern "C" {
  char __attribute__ ((visibility ("default"))) SWELL_WANT_LOAD_LIBRARY[] = STR2(SWELL_PRELOAD);
};
#undef STR
#undef STR2
#endif

#ifdef SWELL_TARGET_GDK

#include "swell-internal.h"
#include <limits.h>
#include <gdk/gdkkeysyms.h>
#include "swell-gdk.h"
#include "swell-dlggen.h"
#include "../wdlcstring.h"
#include "../wdlutf8.h"


#ifdef SWELL_SUPPORT_GTK
void swell_im_update_candidates_location();
extern HWND swell_ime_target;
extern GtkIMContext *swell_ime_context;
#endif


// for m_oswindow_private
#define PRIVATE_NEEDSHOW 1 


#ifndef SWELL_WINDOWSKEY_GDK_MASK
#define  SWELL_WINDOWSKEY_GDK_MASK GDK_MOD4_MASK
#endif

static int SWELL_gdk_active; // -1 fail, 1=GDK, 2=GTK+ loaded, 3=GTK+ falied
GdkEvent *swell_gdk_current_event;
const SWELL_GdkBackend *swell_gdk_backend = &swell_gdk_x11;
static GList *s_program_icon_list;

HWND swell_gdk_drag_source;
DWORD swell_lastMessagePos;
static const char *swell_dragsrc_fn;


static int gdk_options;
#define OPTION_KEEP_OWNED_ABOVE 1
#define OPTION_OWNED_TASKLIST 2
#define OPTION_BORDERLESS_OVERRIDEREDIRECT 4
#define OPTION_BORDERLESS_DIALOG 8
#define OPTION_ALLOW_MAYBE_INACTIVE 16
#define OPTION_FULLSCREEN_FOR_OWNER_WINDOWS 32
#define OPTION_FULLSCREEN_DYNAMIC 64
#define OPTION_IS_WAYLAND 0x20000000
#define OPTIONS_READ 0x40000000


static SWELL_CursorResourceIndex *SWELL_curmodule_cursorresource_head;

static int s_cursor_vis_cnt;

static HCURSOR s_last_cursor;
static HCURSOR s_last_setcursor;
static SWELL_OSWINDOW s_last_setcursor_oswnd;

static void *g_swell_touchptr; // last GDK touch sequence
static void *g_swell_touchptr_wnd; // last window of touch sequence, for forcing end of sequence on destroy

static bool g_swell_mouse_relmode;
static int g_swell_mouse_relmode_curpos_x;
static int g_swell_mouse_relmode_curpos_y;


static void swell_gdkEventHandler(GdkEvent *event, gpointer data);

static UINT_PTR s_deactivate_timer;
static guint32 s_force_window_time;
static bool swell_app_is_inactive;

int swell_is_app_inactive()
{
  return swell_app_is_inactive ? 1 : (gdk_options&OPTION_ALLOW_MAYBE_INACTIVE) && s_deactivate_timer!=0 ? -1 : 0;
}

static void update_menubar_activations()
{
  if (g_swell_ctheme.menubar_bg == g_swell_ctheme.menubar_bg_inactive &&
      g_swell_ctheme.menubar_text == g_swell_ctheme.menubar_text_inactive) return;

  HWND h = SWELL_topwindows;
  while (h)
  {
    if (h->m_oswindow && h->m_menu)
    {
      DrawMenuBar(h);
    }
    h=h->m_next;
  }
}

static void on_activate(guint32 ftime)
{
  s_force_window_time = ftime;
  swell_app_is_inactive=false;
  HWND h = SWELL_topwindows; 
  while (h)
  {
    if (h->m_oswindow)
    {
      if (h->m_israised)
        gdk_window_set_keep_above(h->m_oswindow,TRUE);

      if (!h->m_enabled) 
        gdk_window_set_accept_focus(h->m_oswindow,FALSE);
    }

    PostMessage(h,WM_ACTIVATEAPP,1,0);
    h=h->m_next;
  }
  swell_gdk_backend->activate(true);
  s_force_window_time = 0;

  update_menubar_activations();
}

void swell_gdk_reactivate_app(void)
{
  if (swell_app_is_inactive)
  {
    SWELL_focused_oswindow=NULL;
    on_activate(GDK_CURRENT_TIME);
  }
}

static void on_deactivate()
{
  swell_app_is_inactive=true;
  swell_gdk_backend->activate(false);

  HWND h = SWELL_topwindows; 
  while (h)
  {
    if (h->m_oswindow)
    {
      if (h->m_israised)
        gdk_window_set_keep_above(h->m_oswindow,FALSE);
      if (!h->m_enabled) 
        gdk_window_set_accept_focus(h->m_oswindow,TRUE); // allow the user to activate app by clicking
    }
    PostMessage(h,WM_ACTIVATEAPP,0,0);
    h=h->m_next;
  }
  swell_on_toplevel_raise(NULL);
  DestroyPopupMenus();
}

void swell_oswindow_destroy(HWND hwnd)
{
  if (hwnd && hwnd->m_oswindow)
  {
    if (SWELL_focused_oswindow == hwnd->m_oswindow) SWELL_focused_oswindow = NULL;
    if (g_swell_touchptr && g_swell_touchptr_wnd == hwnd->m_oswindow)
      g_swell_touchptr = NULL;
    swell_gdk_backend->destroy_window(hwnd);
    hwnd->m_oswindow=NULL;
#ifdef SWELL_LICE_GDI
    delete hwnd->m_backingstore;
    hwnd->m_backingstore=0;
#endif

    if (swell_app_is_inactive)
    {
      HWND h = SWELL_topwindows;
      while (h)
      {
        if (h->m_oswindow) break;
        h = h->m_next;
      }
      if (!h) on_activate(10); // arbitrary old timestamp that is nonzero
    }
  }
}
void swell_oswindow_update_text(HWND hwnd)
{
  if (hwnd && hwnd->m_oswindow)
  {
    gdk_window_set_title(hwnd->m_oswindow, (char*)hwnd->m_title.Get());
  }
}


void swell_oswindow_focus(HWND hwnd)
{
  if (!hwnd)
  {
    SWELL_focused_oswindow = NULL;
    update_menubar_activations();
    return;
  }

  while (hwnd && !hwnd->m_oswindow) hwnd=hwnd->m_parent;
  if (hwnd && !swell_app_is_inactive)
  {
    if (swell_gdk_backend->focus_window(hwnd, !(gdk_options & OPTION_IS_WAYLAND),
                                       (gdk_options & OPTION_BORDERLESS_OVERRIDEREDIRECT) != 0))
      update_menubar_activations();
  }
}

void swell_gdk_close(HWND hwnd)
{
  if (!hwnd) return;
  hwnd->Retain();
  if (swell_gdk_backend->close_window(hwnd) && IsWindowEnabled(hwnd) &&
      !DestroyPopupMenus() && // ignore if a menu is open, instead just close the menu
      !SendMessage(hwnd,WM_CLOSE,0,0))
    SendMessage(hwnd,WM_COMMAND,IDCANCEL,0);
  hwnd->Release();
}

bool swell_gdk_size_hints(HWND hwnd, const RECT *fixed, GdkGeometry *hints)
{
  if (fixed)
  {
    memset(hints, 0, sizeof(*hints));
    hints->min_width = hints->max_width = fixed->right - fixed->left;
    hints->min_height = hints->max_height = fixed->bottom - fixed->top;
    return true;
  }
  MINMAXINFO mmi;
  memset(&mmi,0,sizeof(mmi));
  if (hwnd->m_style & WS_THICKFRAME)
  {
    mmi.ptMinTrackSize.x = 20;
    mmi.ptMaxSize.x = mmi.ptMaxTrackSize.x = 16384;
    mmi.ptMinTrackSize.y = 20;
    mmi.ptMaxSize.y = mmi.ptMaxTrackSize.y = 16384;
    hwnd->Retain();
    SendMessage(hwnd,WM_GETMINMAXINFO,0,(LPARAM)&mmi);
    const bool destroyed = hwnd->m_hashaddestroy != 0;
    hwnd->Release();
    if (destroyed) return false;
  }
  else
  {
    RECT r=hwnd->m_position;
    mmi.ptMinTrackSize.x = mmi.ptMaxSize.x = mmi.ptMaxTrackSize.x = r.right-r.left;
    mmi.ptMinTrackSize.y = mmi.ptMaxSize.y = mmi.ptMaxTrackSize.y = r.bottom-r.top;
  }

  GdkGeometry &h = *hints;
  memset(&h,0,sizeof(h));
  h.max_width= mmi.ptMaxSize.x;
  h.max_height= mmi.ptMaxSize.y;
  h.min_width= mmi.ptMinTrackSize.x;
  h.min_height= mmi.ptMinTrackSize.y;
  return true;
}

void swell_recalcMinMaxInfo(HWND hwnd)
{
  if (hwnd && hwnd->m_oswindow && (hwnd->m_style & WS_CAPTION))
    swell_gdk_backend->set_size_hints(hwnd, NULL);
}

#ifdef SWELL_SUPPORT_GTK
gboolean (*swell_gtk_init_check)(int *argc, char ***argv);
void (*swell_gtk_main_do_event)(GdkEvent *);
gboolean (*swell_gtk_im_context_filter_keypress)(GtkIMContext *context, GdkEventKey *event);
void (*swell_gtk_im_context_set_cursor_location)(GtkIMContext *, const GdkRectangle *);
GtkIMContext *(*swell_gtk_im_multicontext_new)(void);
void (*swell_gtk_im_context_set_client_window)(GtkIMContext *context, GdkWindow *window);
void (*swell_gtk_im_context_get_preedit_string)(GtkIMContext *, gchar **, PangoAttrList **, gint *);
void (*swell_gtk_im_context_focus_in)(GtkIMContext *);
void (*swell_gtk_im_context_focus_out)(GtkIMContext *);
void (*swell_gtk_im_context_set_surrounding)(GtkIMContext *context,  const gchar *text, gint len, gint cursor_index);
#endif

#ifdef SWELL_SUPPORT_GTK
bool SWELL_load_gtk(void)
{
  if (SWELL_gdk_active != 1) return SWELL_gdk_active == 2;

  if (dlopen("libgtk-3.so.0",RTLD_NOW|RTLD_GLOBAL) || dlopen("libgtk+-3.so.0",RTLD_NOW|RTLD_GLOBAL))
  {
    *(void **)&swell_gtk_init_check = dlsym(RTLD_DEFAULT,"gtk_init_check");
    *(void **)&swell_gtk_main_do_event = dlsym(RTLD_DEFAULT,"gtk_main_do_event");
    *(void **)&swell_gtk_im_context_filter_keypress = dlsym(RTLD_DEFAULT,"gtk_im_context_filter_keypress");
    *(void **)&swell_gtk_im_context_set_cursor_location = dlsym(RTLD_DEFAULT, "gtk_im_context_set_cursor_location");
    *(void **)&swell_gtk_im_multicontext_new = dlsym(RTLD_DEFAULT, "gtk_im_multicontext_new");
    *(void **)&swell_gtk_im_context_set_client_window = dlsym(RTLD_DEFAULT, "gtk_im_context_set_client_window");
    *(void **)&swell_gtk_im_context_get_preedit_string = dlsym(RTLD_DEFAULT, "gtk_im_context_get_preedit_string");
    *(void **)&swell_gtk_im_context_focus_in = dlsym(RTLD_DEFAULT, "gtk_im_context_focus_in");
    *(void **)&swell_gtk_im_context_focus_out = dlsym(RTLD_DEFAULT, "gtk_im_context_focus_out");
    *(void **)&swell_gtk_im_context_set_surrounding = dlsym(RTLD_DEFAULT, "gtk_im_context_set_surrounding");
  }

  if (swell_gtk_init_check && swell_gtk_main_do_event)
  {
    int argc = 1;
    char buf[32];
    strcpy(buf,"blah");
    char *argv[1] = { buf };
    char **argvv = argv;

    void (*disable_setlocale)(void);
    *(void **)&disable_setlocale = dlsym(RTLD_DEFAULT, "gtk_disable_setlocale");
    if (disable_setlocale) disable_setlocale();

    SWELL_gdk_active = swell_gtk_init_check(&argc,&argvv) ? 2 : 3;
    if (SWELL_gdk_active == 3)
      printf("swell-generic-gdk: GTK+ requested but initialization failed.\n");
    else
      printf("swell-generic-gdk: initialized GTK+ on request.\n");
    gdk_event_handler_set(swell_gdkEventHandler,NULL,NULL);
  }
  else
  {
    printf("swell-generic-gdk: GTK+ requested but not found\n");
    swell_gtk_main_do_event = NULL;
    swell_gtk_init_check = NULL;
    swell_gtk_im_context_set_cursor_location = NULL;
    swell_gtk_im_context_filter_keypress = NULL;
    swell_gtk_im_context_set_client_window = NULL;
    swell_gtk_im_multicontext_new = NULL;
    swell_gtk_im_context_get_preedit_string = NULL;
    swell_gtk_im_context_focus_in = NULL;
    swell_gtk_im_context_focus_out = NULL;
    swell_gtk_im_context_set_surrounding = NULL;
    SWELL_gdk_active = 3;
  }
  return SWELL_gdk_active == 2;
}
#endif

void SWELL_initargs(int *argc, char ***argv) 
{
  if (!SWELL_gdk_active) 
  {
    swell_x11_init();
#if SWELL_TARGET_GDK == 3
    void (*_gdk_set_allowed_backends)(const char *);

    *(void **)&_gdk_set_allowed_backends = dlsym(RTLD_DEFAULT,"gdk_set_allowed_backends");

#ifdef SWELL_TARGET_WAYLAND
      _gdk_set_allowed_backends("wayland,x11");
#else
      _gdk_set_allowed_backends("x11");
#endif
#endif

#ifdef SWELL_SUPPORT_GTK
    dlopen("libgdk-3.so.0", RTLD_NOW | RTLD_GLOBAL);
#endif

    if (!SWELL_gdk_active)
      SWELL_gdk_active = gdk_init_check(argc,argv) ? 1 : -1;
#ifdef SWELL_TARGET_WAYLAND
    if (SWELL_gdk_active > 0 && !swell_wayland_init_display()) SWELL_gdk_active = -1;
#endif
    if (SWELL_gdk_active > 0)
    {
      char buf[1024];
      GetModuleFileName(NULL,buf,sizeof(buf));
      WDL_remove_filepart(buf);
      lstrcatn(buf,"/Resources/main.png",sizeof(buf));
      GdkPixbuf *pb = gdk_pixbuf_new_from_file(buf,NULL);
      if (!pb)
      {
        strcpy(buf+strlen(buf)-3,"ico");
        pb = gdk_pixbuf_new_from_file(buf,NULL);
      }
      if (pb) s_program_icon_list = g_list_append(s_program_icon_list,pb);

      gdk_event_handler_set(swell_gdkEventHandler,NULL,NULL);
    }
  }
}

static bool swell_initwindowsys()
{
  if (!SWELL_gdk_active) 
  {
   // maybe make the main app call this with real parms
    int argc=1;
    char buf[32];
    strcpy(buf,"blah");
    char *argv[2];
    argv[0] = buf;
    argv[1] = buf;
    char **pargv = argv;
    SWELL_initargs(&argc,&pargv);
  }
  
  return SWELL_gdk_active>0;
}

#ifdef SWELL_LICE_GDI
class LICE_CairoBitmap : public LICE_IBitmap
{
  public:
    LICE_CairoBitmap() 
    {
      m_fb = NULL; 
      m_allocsize = m_width = m_height = m_span = 0;
      m_surf = NULL;
    }
    virtual ~LICE_CairoBitmap() 
    { 
      if (m_surf) cairo_surface_destroy(m_surf);
      free(m_fb);
    }

    // LICE_IBitmap interface
    virtual LICE_pixel *getBits() 
    { 
      const UINT_PTR extra=LICE_MEMBITMAP_ALIGNAMT;
      return (LICE_pixel *) (((UINT_PTR)m_fb + extra)&~extra);
    }

    virtual int getWidth() { return m_width; }
    virtual int getHeight() { return m_height; }
    virtual int getRowSpan() { return m_span; }
    virtual bool resize(int w, int h)
    {
      if (w<0) w=0; 
      if (h<0) h=0;
      if (w == m_width && h == m_height) return false;

      const int span = w ? cairo_format_stride_for_width(CAIRO_FORMAT_RGB24,w)/4 : 0;
      const WDL_INT64 size = (WDL_INT64)h * span * 4 + LICE_MEMBITMAP_ALIGNAMT;
      if (span < w || size > INT_MAX) return false;

      if (m_surf) cairo_surface_destroy(m_surf);
      m_surf = NULL;

      const int sz = (int)size;
      if (!m_fb || m_allocsize < sz || sz < m_allocsize/4)
      {
        const int newalloc = m_allocsize<sz ? sz + wdl_min(sz/2, INT_MAX-sz) : sz;
        void *p = realloc(m_fb,newalloc);
        if (!p) return false;

        m_fb = (LICE_pixel *)p;
        m_allocsize = newalloc;
      }

      m_span = span;
      m_width=w && h ? w :0;
      m_height=w && h ? h : 0;
      return true;
    }
    virtual INT_PTR Extended(int id, void* data) 
    { 
      if (id == 0xca140) 
      {
        if (data) 
        {
          // in case we want to release surface
          return 0;
        }

        if (!m_surf) 
          m_surf = cairo_image_surface_create_for_data((guchar*)getBits(), CAIRO_FORMAT_RGB24, 
                                                       getWidth(),getHeight(), getRowSpan()*4);
        return (INT_PTR)m_surf;
      }
      return 0; 
    }

private:
  LICE_pixel *m_fb;
  int m_width, m_height, m_span;
  int m_allocsize;
  cairo_surface_t *m_surf;
};
#endif

static int swell_gdk_option(const char *name, const char *defstr, int defv)
{
  char buf[64];
  GetPrivateProfileString(".swell",name,"",buf,sizeof(buf),"");
  if (!buf[0]) WritePrivateProfileString(".swell",name,defstr,"");
  if (buf[0] >= '0' && buf[0] <= '9') return atoi(buf);
  return defv;
}

static void init_options()
{
  if (!gdk_options)
  {
    gdk_options = OPTIONS_READ;
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if (wayland_display && *wayland_display) gdk_options |= OPTION_IS_WAYLAND;

#ifdef SWELL_TARGET_WAYLAND
    if (swell_is_wayland()) gdk_options |= OPTION_IS_WAYLAND;
#endif

    if (swell_gdk_option("gdk_owned_windows_keep_above", "auto (default is 1)",1))
      gdk_options|=OPTION_KEEP_OWNED_ABOVE;

    if (swell_gdk_option("gdk_owned_windows_in_tasklist", "auto (default is 0)",0))
      gdk_options|=OPTION_OWNED_TASKLIST;

    switch (swell_gdk_option("gdk_instant_menubar_inactivation", "auto (default is 1 if on Wayland, otherwise 0)",-1))
    {
      case -1:
        if (!(gdk_options & OPTION_IS_WAYLAND)) break;
        // fall through
      case 1:
        gdk_options|=OPTION_ALLOW_MAYBE_INACTIVE;
      break;
    }

    switch (swell_gdk_option("gdk_borderless_window_mode", "auto (default is 1=dialog hint. 2=override redirect. 0=normal hint)", 1))
    {
      case 1: gdk_options|=OPTION_BORDERLESS_DIALOG; break;
      case 2: gdk_options|=OPTION_BORDERLESS_OVERRIDEREDIRECT; break;
      default: break;
    }

    const char *wmname = swell_gdk_backend->window_manager_name();
    switch (swell_gdk_option("gdk_fullscreen_for_owner_windows", "auto (default is 1 on kwin, otherwise 0)",-1))
    {
      case -1:
        if (!wmname || strnicmp(wmname,"KWin",4)) break;
        // fall through
      case 1:
        gdk_options |= OPTION_FULLSCREEN_FOR_OWNER_WINDOWS;
      break;
    }
    switch (swell_gdk_option("gdk_fullscreen_dynamic","auto (default is 1 on kwin, otherwise 0)",-1))
    {
      case -1:
        if (!wmname || strnicmp(wmname,"KWin",4)) break;
        // fall through
      case 1:
        gdk_options |= OPTION_FULLSCREEN_DYNAMIC;
      break;

    }
  }
  
}

static void swell_hide_owned_windows_transient(HWND hwnd)
{
  if ((gdk_options&OPTION_KEEP_OWNED_ABOVE) && hwnd->m_owned_list)
  {
    HWND l = SWELL_topwindows;
    while (l)
    {
      if (l->m_oswindow && l->m_owner == hwnd && l->m_visible)
        gdk_window_hide(l->m_oswindow);
      l = l->m_next;
    }
  }
}

static void swell_set_owned_windows_transient(HWND hwnd, bool do_create)
{
  if ((gdk_options&OPTION_KEEP_OWNED_ABOVE) && hwnd->m_owned_list)
  {
    WDL_PtrList<void> raiselist;
    HWND l = SWELL_topwindows;
    while (l)
    {
      if (l->m_owner == hwnd && l->m_visible)
      {
        if (l->m_oswindow) raiselist.Add(l->m_oswindow);
        else if (do_create) swell_oswindow_manage(l,false);
      }
      l = l->m_next;
    }
    for (int x = raiselist.GetSize()-1; x>=0; x--)
    {
      SWELL_OSWINDOW r = (SWELL_OSWINDOW)raiselist.Get(x);
      gdk_window_set_transient_for(r,hwnd->m_oswindow);
      gdk_window_show_unraised(r);
    }
  }
}


bool IsModalDialogBox(HWND);

void swell_oswindow_manage(HWND hwnd, bool wantfocus)
{
  if (!hwnd) return;

  bool isVis = hwnd->m_oswindow != NULL;
  bool wantVis = !hwnd->m_parent && hwnd->m_visible;

  if (isVis != wantVis)
  {
    if (!wantVis) 
    {
      RECT r;
      GetWindowRect(hwnd,&r);
      swell_hide_owned_windows_transient(hwnd);
      swell_oswindow_destroy(hwnd);
      hwnd->m_position = r;
    }
    else 
    {
      if (swell_initwindowsys())
      {
        init_options();

        SWELL_OSWINDOW transient_for=NULL;
        if (hwnd->m_owner && (gdk_options&OPTION_KEEP_OWNED_ABOVE))
        {
          HWND own = hwnd->m_owner;
          while (own->m_parent && !own->m_oswindow) own=own->m_parent;

          if (!own->m_oswindow)
          { 
            if (!IsModalDialogBox(hwnd)) return; // defer

            // if a modal window, parent to any owner up the chain
            while (own->m_owner && !own->m_oswindow)
            {
              own = own->m_owner;
              while (own->m_parent && !own->m_oswindow) own=own->m_parent;
            }
          }
          transient_for = own->m_oswindow;
        }

        RECT r = hwnd->m_position;
        if (!swell_gdk_backend->create_window(hwnd, transient_for)) return;

        if (hwnd->m_oswindow) 
        {
          bool override_redirect=false;
          const bool modal = DialogBoxIsActive() == hwnd;

          if (!(hwnd->m_style & WS_CAPTION)) 
          {
            // WS_CHILD is used by menus to force override redirect
            if (hwnd->m_style != WS_CHILD && !(gdk_options&OPTION_BORDERLESS_OVERRIDEREDIRECT))
            {
              if (transient_for)
                gdk_window_set_transient_for(hwnd->m_oswindow,transient_for);
              gdk_window_set_type_hint(hwnd->m_oswindow, (gdk_options&OPTION_BORDERLESS_DIALOG) ? GDK_WINDOW_TYPE_HINT_DIALOG : GDK_WINDOW_TYPE_HINT_NORMAL);
              gdk_window_set_decorations(hwnd->m_oswindow,(GdkWMDecoration) 0);
            }
            else
            {
              gdk_window_set_override_redirect(hwnd->m_oswindow,true);
              override_redirect=true;
            }
            if (!SWELL_topwindows || 
                (SWELL_topwindows==hwnd && !hwnd->m_next)) wantfocus=true;
          }
          else 
          {
            GdkWindowTypeHint type_hint = GDK_WINDOW_TYPE_HINT_NORMAL;
            GdkWMDecoration decor = (GdkWMDecoration) (GDK_DECOR_ALL | GDK_DECOR_MENU);

            if (!(hwnd->m_style&WS_THICKFRAME))
              decor = (GdkWMDecoration) (GDK_DECOR_BORDER|GDK_DECOR_TITLE|GDK_DECOR_MINIMIZE);

            if (transient_for)
            {
              gdk_window_set_transient_for(hwnd->m_oswindow,transient_for);
            }

            if (modal) type_hint = GDK_WINDOW_TYPE_HINT_DIALOG;

            gdk_window_set_type_hint(hwnd->m_oswindow,type_hint);
            gdk_window_set_decorations(hwnd->m_oswindow,decor);
          }

          if (!wantfocus || swell_app_is_inactive)
            gdk_window_set_focus_on_map(hwnd->m_oswindow,false);

#ifdef SWELL_LICE_GDI
          if (!hwnd->m_backingstore) hwnd->m_backingstore = new LICE_CairoBitmap;
#endif
          if (!override_redirect)
          {
            if (s_program_icon_list) 
              gdk_window_set_icon_list(hwnd->m_oswindow,s_program_icon_list);
          }
          if (hwnd->m_owner && !(gdk_options&OPTION_OWNED_TASKLIST) && !override_redirect)
          {
            gdk_window_set_skip_taskbar_hint(hwnd->m_oswindow,true);
          }
          else if (hwnd->m_style == WS_CHILD) 
          {
            // hack: parentless visible window with WS_CHILD set will 
            // not appear in taskbar
            gdk_window_set_skip_taskbar_hint(hwnd->m_oswindow,true);
          }

          if (hwnd->m_israised && !swell_app_is_inactive)
            gdk_window_set_keep_above(hwnd->m_oswindow,TRUE);

          if (hwnd->m_oswindow_fullscreen)
            gdk_window_fullscreen(hwnd->m_oswindow);

          swell_gdk_backend->show_window(hwnd, !swell_app_is_inactive && !s_force_window_time,
              modal && transient_for && (hwnd->m_style & WS_CAPTION), s_force_window_time);

          if (!hwnd->m_oswindow_fullscreen)
          {
            swell_oswindow_resize(hwnd->m_oswindow,hwnd->m_has_had_position?3:2,r);
            swell_oswindow_postresize(hwnd,r);
          }

          swell_set_owned_windows_transient(hwnd, true);
        }
      }
    }
  }
  if (wantVis) swell_oswindow_update_text(hwnd);
}

void swell_oswindow_maximize(HWND hwnd, bool wantmax) // false=restore
{
  if (WDL_NOT_NORMALLY(!hwnd)) return;
  if (!hwnd->m_oswindow)
  {
    hwnd->m_is_maximized = wantmax;
    return;
  }
  if (wantmax)
    gdk_window_maximize(hwnd->m_oswindow);
  else
    gdk_window_unmaximize(hwnd->m_oswindow);
}

void swell_oswindow_updatetoscreen(HWND hwnd, RECT *rect)
{
  swell_gdk_backend->update_window(hwnd, rect);
}

#if SWELL_TARGET_GDK == 2
  #define DEF_GKY(x) GDK_##x
#else
  #define DEF_GKY(x) GDK_KEY_##x
#endif

static guint swell_gdkConvertKey(guint key, bool *extended)
{
  switch(key)
  {
#define DEF_GK2(h, v)  \
    case DEF_GKY(h): *extended = true; return (v);  \
    case DEF_GKY(KP_##h): return (v);

    DEF_GK2(Home,VK_HOME)
    DEF_GK2(End,VK_END)
    DEF_GK2(Up, VK_UP)
    DEF_GK2(Down, VK_DOWN)
    DEF_GK2(Left, VK_LEFT)
    DEF_GK2(Right, VK_RIGHT)
    DEF_GK2(Page_Up, VK_PRIOR)
    DEF_GK2(Page_Down, VK_NEXT)
    DEF_GK2(Insert,VK_INSERT)
    DEF_GK2(Delete, VK_DELETE)

#undef DEF_GK2
    case DEF_GKY(KP_Enter): *extended = true; return VK_RETURN;
    case DEF_GKY(Return): return VK_RETURN;

    case DEF_GKY(Escape): return VK_ESCAPE;
    case DEF_GKY(BackSpace): return VK_BACK;
    case DEF_GKY(ISO_Left_Tab):
    case DEF_GKY(Tab): return VK_TAB;
    case DEF_GKY(F1): return VK_F1;
    case DEF_GKY(F2): return VK_F2;
    case DEF_GKY(F3): return VK_F3;
    case DEF_GKY(F4): return VK_F4;
    case DEF_GKY(F5): return VK_F5;
    case DEF_GKY(F6): return VK_F6;
    case DEF_GKY(F7): return VK_F7;
    case DEF_GKY(F8): return VK_F8;
    case DEF_GKY(F9): return VK_F9;
    case DEF_GKY(F10): return VK_F10;
    case DEF_GKY(F11): return VK_F11;
    case DEF_GKY(F12): return VK_F12;
    case DEF_GKY(KP_0): return VK_NUMPAD0;
    case DEF_GKY(KP_1): return VK_NUMPAD1;
    case DEF_GKY(KP_2): return VK_NUMPAD2;
    case DEF_GKY(KP_3): return VK_NUMPAD3;
    case DEF_GKY(KP_4): return VK_NUMPAD4;
    case DEF_GKY(KP_5): return VK_NUMPAD5;
    case DEF_GKY(KP_6): return VK_NUMPAD6;
    case DEF_GKY(KP_7): return VK_NUMPAD7;
    case DEF_GKY(KP_8): return VK_NUMPAD8;
    case DEF_GKY(KP_9): return VK_NUMPAD9;
    case DEF_GKY(KP_Multiply): return VK_MULTIPLY;
    case DEF_GKY(KP_Add): return VK_ADD;
    case DEF_GKY(KP_Separator): return VK_SEPARATOR;
    case DEF_GKY(KP_Subtract): return VK_SUBTRACT;
    case DEF_GKY(KP_Decimal): return VK_DECIMAL;
    case DEF_GKY(KP_Divide): return VK_DIVIDE;
    case DEF_GKY(Num_Lock): return VK_NUMLOCK;
    case DEF_GKY(KP_Begin): return VK_SELECT;
  }
  return 0;
}


cairo_surface_t *swell_gdk_surface(HWND hwnd)
{
#ifdef SWELL_LICE_GDI
  LICE_IBitmap *bm = hwnd->m_backingstore;
  if (bm)
  {
    cairo_surface_t *surface = (cairo_surface_t*)bm->Extended(0xca140, NULL);
    if (surface)
    {
      cairo_surface_reference(surface);
      bm->Extended(0xca140, surface);
      return surface;
    }
  }
#endif
  return NULL;
}

cairo_surface_t *swell_gdk_paint(HWND hwnd, const GdkRectangle &area)
{
#ifdef SWELL_LICE_GDI
  RECT r,cr;

  // don't use GetClientRect(),since we're getting it pre-NCCALCSIZE etc

  cr.left=cr.top=0;
  cr.right = hwnd->m_position.right - hwnd->m_position.left;
  cr.bottom = hwnd->m_position.bottom - hwnd->m_position.top;

  r.left = area.x;
  r.top = area.y;
  r.bottom=r.top+area.height;
  r.right=r.left+area.width;
  if (r.left < cr.left) r.left = cr.left;
  if (r.right > cr.right) r.right = cr.right;
  if (r.top < cr.top) r.top = cr.top;
  if (r.bottom > cr.bottom) r.bottom = cr.bottom;

  if (!hwnd->m_backingstore) hwnd->m_backingstore = new LICE_CairoBitmap;

  bool forceref = hwnd->m_backingstore->resize(cr.right-cr.left,cr.bottom-cr.top);
  if (forceref) r = cr;

  LICE_SubBitmap tmpbm(hwnd->m_backingstore,r.left,r.top,r.right-r.left,r.bottom-r.top);

  if (tmpbm.getWidth() <= 0 || tmpbm.getHeight() <= 0) return NULL;
  void SWELL_internalLICEpaint(HWND, LICE_IBitmap *, int, int, bool);
  hwnd->Retain();
  SWELL_internalLICEpaint(hwnd, &tmpbm, r.left, r.top, forceref);
  const bool destroyed = hwnd->m_hashaddestroy != 0;
  hwnd->Release();
  if (destroyed) return NULL;
  return swell_gdk_surface(hwnd);
#else
  return NULL;
#endif
}

void swell_gdk_configure(GdkEventConfigure *cfg)
{
  HWND hwnd = swell_oswindow_to_hwnd(cfg->window);
  if (!hwnd) return;
  int cfg_x = cfg->x, cfg_y = cfg->y;
  int cfg_width = cfg->width, cfg_height = cfg->height;
  int flag=0;
  if (cfg_x != hwnd->m_position.left || 
      cfg_y != hwnd->m_position.top || 
      !hwnd->m_has_had_position)
  {
    flag|=1;
    hwnd->m_has_had_position = true;
  }
  if (cfg_width != hwnd->m_position.right-hwnd->m_position.left || 
      cfg_height != hwnd->m_position.bottom - hwnd->m_position.top) flag|=2;
  hwnd->m_position.left = cfg_x;
  hwnd->m_position.top = cfg_y;
  hwnd->m_position.right = cfg_x + cfg_width;
  hwnd->m_position.bottom = cfg_y + cfg_height;
  hwnd->Retain();
  if (flag&1) SendMessage(hwnd,WM_MOVE,0,0);
  if (flag&2) SendMessage(hwnd,WM_SIZE,hwnd->m_is_maximized ? SIZE_MAXIMIZED : SIZE_RESTORED,0);
  if (!hwnd->m_hashaddestroy && hwnd->m_oswindow && (hwnd->m_style & WS_THICKFRAME))
    swell_recalcMinMaxInfo(hwnd);
  hwnd->Release();
}

static void OnWindowStateEvent(GdkEventWindowState *evt)
{
  HWND hwnd = swell_oswindow_to_hwnd(evt->window);
  if (!hwnd) return;

  if (evt->changed_mask & GDK_WINDOW_STATE_MAXIMIZED)
  {
    const bool maximized = (evt->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    hwnd->m_is_maximized = maximized;
    SendMessage(hwnd,WM_SIZE,
        maximized ? SIZE_MAXIMIZED : SIZE_RESTORED, 0);
  }
}

#ifndef GDK_KEY_dead_greek
#define GDK_KEY_dead_greek 0xfe8c
#endif
#define IS_DEAD_KEY(x) ((x) >= DEF_GKY(dead_grave) && (x) <= GDK_KEY_dead_greek)

#include "gtkimcontextsimpleseqs.h"

static guint find_compose_sequence_compact(const guint16 *state, int state_size) // returns 0 if sequence not found, 1 if partial match, otherwise unicode character
{
  WDL_ASSERT(state_size > 0);

  const int max_seq_len = 5;
  if (WDL_NOT_NORMALLY(state_size > max_seq_len)) return 0;

  const int index_size = 30, index_stride = 6;
  const guint16 fc = state[0];
  const guint16 *index = gtk_compose_seqs_compact;

  int x;
  for (x = 0; x < index_size && index[0] != fc; x ++, index += index_stride);
  if (x == index_size) return 0;
  if (!--state_size) return 1;
  state++;

  x = index[1];
  for (int l = 2; l < index_stride; l ++)
  {
    const int mv = index[l];
    const int sl = l-1;
    while (x < mv)
    {
      WDL_ASSERT(x >= index_stride * index_size);
      WDL_ASSERT(x < (int) (sizeof(gtk_compose_seqs_compact) / sizeof(gtk_compose_seqs_compact[0])));
      // gtk_compose_seqs_compact+x is the remaining sequence
      if (state_size <= sl && !memcmp(state,gtk_compose_seqs_compact+x,state_size*sizeof(state[0])))
      {
        return state_size == sl ? gtk_compose_seqs_compact[x + sl] : 1;
      }
      x += l;
    }
  }

  return 0;
}

static guint find_compose_sequence_alg(const guint16 *state, int state_size) // returns 0 if sequence not found, 1 if partial match, otherwise unicode character
{
  if (state_size < 2) return state_size==1 && IS_DEAD_KEY(state[0]) ? 1 : 0;
  int i;
  for (i = 0; i < state_size && IS_DEAD_KEY(state[i]); i++);
  if (i != state_size-1) return 0; // must be all dead keys + one non-dead

  gunichar uc = gdk_keyval_to_unicode(state[state_size-1]);
  if (!g_unichar_isalpha(uc)) return 0;

  // this is borrowed from GTK+'s impl
  GString *input = g_string_sized_new(4 * state_size);
  g_string_append_unichar(input, uc);
  while (--i >= 0)
  {
    switch (state[i])
    {
#define CASE(keysym, unicode) case DEF_GKY(dead_##keysym): g_string_append_unichar(input, unicode); break
      CASE (grave, 0x0300);
      CASE (acute, 0x0301);
      CASE (circumflex, 0x0302);
      case GDK_KEY_dead_tilde:
        if (g_unichar_get_script (uc) == G_UNICODE_SCRIPT_GREEK)
          g_string_append_unichar (input, 0x342); /* combining perispomeni */
        else
          g_string_append_unichar (input, 0x303); /* combining tilde */
        break;
      CASE (macron, 0x0304);
      CASE (breve, 0x0306);
      CASE (abovedot, 0x0307);
      CASE (diaeresis, 0x0308);
      CASE (abovering, 0x30A);
      CASE (hook, 0x0309);
      CASE (doubleacute, 0x030B);
      CASE (caron, 0x030C);
      CASE (cedilla, 0x0327);
      CASE (ogonek, 0x0328);      /* Legacy use for dasia, 0x314.*/
      CASE (iota, 0x0345);
      CASE (voiced_sound, 0x3099);        /* Per Markus Kuhn keysyms.txt file. */
      CASE (semivoiced_sound, 0x309A);    /* Per Markus Kuhn keysyms.txt file. */
      CASE (belowdot, 0x0323);
      CASE (horn, 0x031B);        /* Legacy use for psili, 0x313 (or 0x343). */
      CASE (stroke, 0x335);
      CASE (abovecomma, 0x0313);  /* Equivalent to psili */
      CASE (abovereversedcomma, 0x0314);   /* Equivalent to dasia */
      CASE (doublegrave, 0x30F);
      CASE (belowring, 0x325);
      CASE (belowmacron, 0x331);
      CASE (belowcircumflex, 0x32D);
      CASE (belowtilde, 0x330);
      CASE (belowbreve, 0x32e);
      CASE (belowdiaeresis, 0x324);
      CASE (invertedbreve, 0x32f);
      CASE (belowcomma, 0x326);
#ifdef GDK_KEY_dead_longsolidusoverlay // requires a particularly recent GTK+3, I guess
      CASE (lowline, 0x332);
      CASE (aboveverticalline, 0x30D);
      CASE (belowverticalline, 0x329);
      CASE (longsolidusoverlay, 0x338);
#endif
      CASE (a, 0x363);
      CASE (A, 0x363);
      CASE (e, 0x364);
      CASE (E, 0x364);
      CASE (i, 0x365);
      CASE (I, 0x365);
      CASE (o, 0x366);
      CASE (O, 0x366);
      CASE (u, 0x367);
      CASE (U, 0x367);
      CASE (small_schwa, 0x1DEA);
      CASE (capital_schwa, 0x1DEA);
#undef CASE
      default:
        g_string_append_unichar (input, gdk_keyval_to_unicode(state[i]));
      break;
    }
  }
  char *nfc = g_utf8_normalize(input->str, input->len, G_NORMALIZE_NFC);

  int rv = 0;
  if (nfc) wdl_utf8_parsechar(nfc,&rv);

  g_string_free(input, TRUE);
  g_free(nfc);

  return rv;
}

static guint swell_gdkComposeKeys(GdkEventKey *k) // return 0 if not in composition, 1 if mid-composition, >1 if composed
{
  const int state_max = 16;
  static guint16 state[state_max], state_pos;
  static GdkWindow *lw;
  if (k->window != lw)
  {
    state_pos=0;
    lw = k->window;
  }

  if (!IS_DEAD_KEY(k->keyval) && !state_pos) return 0;

  static const guint16 mods[] = {
    DEF_GKY(Overlay1_Enable),
    DEF_GKY(Overlay2_Enable),
    DEF_GKY(Shift_L),
    DEF_GKY(Shift_R),
    DEF_GKY(Control_L),
    DEF_GKY(Control_R),
    DEF_GKY(Caps_Lock),
    DEF_GKY(Shift_Lock),
    DEF_GKY(Meta_L),
    DEF_GKY(Meta_R),
    DEF_GKY(Alt_L),
    DEF_GKY(Alt_R),
    DEF_GKY(Super_L),
    DEF_GKY(Super_R),
    DEF_GKY(Hyper_L),
    DEF_GKY(Hyper_R),
    DEF_GKY(Mode_switch),
    DEF_GKY(ISO_Level3_Shift),
    DEF_GKY(ISO_Level3_Latch),
    DEF_GKY(ISO_Level5_Shift),
    DEF_GKY(ISO_Level5_Latch),
  };

  if (!k->keyval) return 1;
  for (size_t x = 0; x < sizeof(mods)/sizeof(mods[0]);x++)
    if (mods[x] == k->keyval) return 1;

  if (k->type == GDK_KEY_PRESS)
  {
    if (k->keyval == DEF_GKY(BackSpace) || k->keyval == DEF_GKY(Escape))
      return 1;

    state[state_pos++] = k->keyval;
  }
  else
  {
    if (k->keyval == DEF_GKY(Escape))
      state_pos = 0;
    else if (k->keyval == DEF_GKY(BackSpace))
      state_pos--;
  }

  if (!state_pos) return 1;

  guint rv = find_compose_sequence_compact(state,state_pos);
  if (rv == 0) rv = find_compose_sequence_alg(state,state_pos);
  if (rv!=1 || WDL_NOT_NORMALLY(state_pos >= state_max)) state_pos = 0;
  return rv ? rv : 1;
}


void swell_gdk_key_event(GdkEventKey *k)
{
  HWND hwnd = swell_oswindow_to_hwnd(k->window);
  if (!hwnd) return;
  int modifiers = 0;
  if (k->state&GDK_CONTROL_MASK) modifiers|=FCONTROL;
  if (k->state&GDK_MOD1_MASK) modifiers|=FALT;
  if (k->state&SWELL_WINDOWSKEY_GDK_MASK) modifiers|=FLWIN;
  if (k->state&GDK_SHIFT_MASK) modifiers|=FSHIFT;

  UINT msgtype = k->type == GDK_KEY_PRESS ? WM_KEYDOWN : WM_KEYUP;

  bool is_extended = false;
  guint kv;

  if ((kv = swell_gdkComposeKeys(k)))
  {
    if (kv==1) return;
    modifiers = 0;
  }
  else if ((kv = swell_gdkConvertKey(k->keyval, &is_extended)))
  {
    modifiers |= FVIRTKEY;

    if (modifiers & FSHIFT)
    {
      if (k->state&GDK_MOD2_MASK) // numlock on
      {
        // if shift+numpad home/end is sent while numlock is on, that means it should be treated
        // as unmodified home/end
        if (!is_extended && kv >= VK_PRIOR && kv <= VK_SELECT)
          modifiers &= ~FSHIFT;
      }
      else
      {
        // if numpadX is sent while numlock is off, then it should be treated as unmodified
        if (kv >= VK_NUMPAD0 && kv <= VK_NUMPAD9)
          modifiers &= ~FSHIFT;
      }
    }
  }
  else 
  {
    kv = k->keyval;
    if (kv >= 'a' && kv <= 'z')
    {
      kv += 'A'-'a';
      swell_is_likely_capslock = (modifiers&FSHIFT)!=0;
      modifiers |= FVIRTKEY;
    }
    else if (kv >= 'A' && kv <= 'Z')
    {
      swell_is_likely_capslock = (modifiers&FSHIFT)==0;
      modifiers |= FVIRTKEY;
    }
    else if (kv == ' ')
    {
      kv = VK_SPACE;
    }
    else if (kv >= '0' && kv <= '9')
    {
      modifiers |= FVIRTKEY;
    }
    else
    {
      if (kv >= DEF_GKY(Shift_L) ||
          (kv >= DEF_GKY(ISO_Lock) &&
           kv <= DEF_GKY(ISO_Last_Group_Lock))
         )
      {
        if (kv == DEF_GKY(Shift_L) || kv == DEF_GKY(Shift_R)) kv = VK_SHIFT;
        else if (kv == DEF_GKY(Control_L) || kv == DEF_GKY(Control_R)) kv = VK_CONTROL;
        else if (kv == DEF_GKY(Alt_L) || kv == DEF_GKY(Alt_R)) kv = VK_MENU;
        else if (kv == DEF_GKY(Super_L) || kv == DEF_GKY(Super_R)) kv = VK_LWIN;
        else return; // unknown modifie key

        msgtype = k->type == GDK_KEY_PRESS ? WM_SYSKEYDOWN : WM_SYSKEYUP;
        modifiers|=FVIRTKEY;
      }
      else if (kv > 255) 
      {
        guint v = gdk_keyval_to_unicode(kv);
        if (v) kv=v;
      }
      else
      {
        // treat as ASCII, clear shift flag (?)
        modifiers &= ~FSHIFT;
      }
    }
  }

  HWND foc = GetFocusIncludeMenus();
  if (foc && IsChild(hwnd,foc)) hwnd=foc;
  else if (foc && foc->m_oswindow && !(foc->m_style&WS_CAPTION)) hwnd=foc; // for menus, event sent to other window due to gdk_window_set_override_redirect()

  if (is_extended) modifiers |= 1<<24;

  MSG msg = { hwnd, msgtype, kv, modifiers, };
  INT_PTR extra_flags = 0;
  if (DialogBoxIsActive()) extra_flags |= 1;

#ifdef SWELL_SUPPORT_GTK
  if (hwnd == swell_ime_target &&
      swell_ime_context &&
      swell_gtk_im_context_filter_keypress &&
      swell_gtk_im_context_filter_keypress(swell_ime_context, k))
        return;
#endif
  if (SWELLAppMain(SWELLAPP_PROCESSMESSAGE,(INT_PTR)&msg,extra_flags)<=0)
    SendMessage(hwnd, msg.message, kv, modifiers);
}

static HWND getMouseTarget(SWELL_OSWINDOW osw, POINT p, const HWND *hwnd_has_osw)
{
  HWND hwnd = GetCapture();
  if (hwnd) return hwnd;
  hwnd = hwnd_has_osw ? *hwnd_has_osw : swell_oswindow_to_hwnd(osw);
  if (!hwnd || swell_window_wants_all_input() == hwnd) return hwnd;
  return ChildWindowFromPoint(hwnd,p);
}


void swell_gdk_motion_event(GdkEventMotion *m)
{
  swell_lastMessagePos = MAKELONG(((int)m->x_root&0xffff),((int)m->y_root&0xffff));

  POINT p={(int)m->x, (int)m->y};
  HWND hwnd = getMouseTarget(m->window,p,NULL);

  if (hwnd)
  {
    POINT p2={(int)m->x_root, (int)m->y_root};
    ScreenToClient(hwnd, &p2);
    if (hwnd) hwnd->Retain();
    SWELL_SendMouseMessage(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(p2.x, p2.y), (int)m->x_root, (int)m->y_root);
    if (hwnd) hwnd->Release();
  }
}

void swell_gdk_scroll_event(GdkEventScroll *b)
{
  swell_lastMessagePos = MAKELONG(((int)b->x_root&0xffff),((int)b->y_root&0xffff));
  POINT p={(int)b->x, (int)b->y};

  HWND hwnd = getMouseTarget(b->window,p,NULL);
  if (hwnd)
  {
    POINT p2={(int)b->x_root, (int)b->y_root};
    // p2 is screen coordinates for WM_MOUSEWHEEL

#if SWELL_TARGET_GDK >= 3
    if (b->direction == GDK_SCROLL_SMOOTH)
    {
      const double dy = b->delta_y * 120.0, dx = b->delta_x * 120.0;
      const int vy = -(int)(dy >= 0 ? dy + 0.5 : dy - 0.5);
      const int vx = -(int)(dx >= 0 ? dx + 0.5 : dx - 0.5);
      if (hwnd) hwnd->Retain();
      if (vy) SWELL_SendMouseMessage(hwnd, WM_MOUSEWHEEL, (vy<<16), MAKELPARAM(p2.x, p2.y), (int)b->x_root, (int)b->y_root);
      if (vx) SWELL_SendMouseMessage(hwnd, WM_MOUSEHWHEEL, (vx<<16), MAKELPARAM(p2.x, p2.y), (int)b->x_root, (int)b->y_root);
      if (hwnd) hwnd->Release();
    }
    else
#endif
    {
    int msg=(b->direction == GDK_SCROLL_UP || b->direction == GDK_SCROLL_DOWN) ? WM_MOUSEWHEEL :
            (b->direction == GDK_SCROLL_LEFT || b->direction == GDK_SCROLL_RIGHT) ? WM_MOUSEHWHEEL : 0;
  
    if (msg) 
    {
      int v = (b->direction == GDK_SCROLL_UP || b->direction == GDK_SCROLL_LEFT) ? 120 : -120;

      if (hwnd) hwnd->Retain();
      SWELL_SendMouseMessage(hwnd, msg, (v<<16), MAKELPARAM(p2.x, p2.y), (int)b->x_root, (int)b->y_root);
      if (hwnd) hwnd->Release();
    }
    }
  }
}

static DWORD s_last_focus_change_time;

void swell_gdk_button_event(GdkEventButton *b)
{
  HWND hwnd = swell_oswindow_to_hwnd(b->window);
  if (!hwnd) return;

  swell_lastMessagePos = MAKELONG(((int)b->x_root&0xffff),((int)b->y_root&0xffff));
  POINT p={(int)b->x, (int)b->y};
  HWND hwnd2 = getMouseTarget(b->window,p,&hwnd);

  POINT p2={(int)b->x_root, (int)b->y_root};
  ScreenToClient(hwnd2, &p2);

  int msg=WM_LBUTTONDOWN;
  if (b->button==2) msg=WM_MBUTTONDOWN;
  else if (b->button==3) msg=WM_RBUTTONDOWN;

  if (hwnd2) hwnd2->Retain();

  if (b->type == GDK_BUTTON_PRESS)
  {
    HWND oldFocus=GetFocus();
    if (!oldFocus || 
        oldFocus != hwnd2 ||
        (GetTickCount()-s_last_focus_change_time) < 500)
    {
      if (IsWindowEnabled(hwnd2))
        SendMessage(hwnd2,WM_MOUSEACTIVATE,0,0);
    }
  }

  if (hwnd && hwnd->m_oswindow && SWELL_focused_oswindow != hwnd->m_oswindow &&
      (b->type != GDK_BUTTON_RELEASE || PopupMenuIsActive()))
  {
    // 'b->type != GDK_BUTTON_RELEASE ||' might not be necessary, the only time this
    // appears to matter is when popup menus are active (focus events aren't sent
    // probably due to the override redirect menu windows)
    SWELL_focused_oswindow = hwnd->m_oswindow;
    update_menubar_activations();
  }


  // for doubleclicks, GDK actually seems to send:
  //   GDK_BUTTON_PRESS, GDK_BUTTON_RELEASE, 
  //   GDK_BUTTON_PRESS, GDK_2BUTTON_PRESS, GDK_BUTTON_RELEASE
  // win32 expects:
  //   WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP
  // what we send:
  //   WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDOWN, WM_LBUTTONUP, 
  //   WM_LBUTTONDBLCLK, WM_LBUTTONUP
  // there is an extra down/up pair, but it should behave fine with most code
  // (one hopes)

  if(b->type == GDK_BUTTON_RELEASE)
  {
   msg++; // convert WM_xBUTTONDOWN to WM_xBUTTONUP
  }
  else if(b->type == GDK_2BUTTON_PRESS) 
  {
    msg++; // convert WM_xBUTTONDOWN to WM_xBUTTONUP
    SWELL_SendMouseMessage(hwnd2, msg, 0, MAKELPARAM(p2.x, p2.y), (int)b->x_root, (int)b->y_root);

    // if capture was released by WM_LBUTTONUP, allow the DBLCLICK to go to the correct window
    HWND hwnd3 = getMouseTarget(b->window,p,&hwnd);
    if (hwnd3 != hwnd2)
    {
      if (hwnd2) hwnd2->Release();
      hwnd2 = hwnd3;
      if (hwnd2) hwnd2->Retain();
      p2.x = (int)b->x_root;
      p2.y = (int)b->y_root;
      ScreenToClient(hwnd2, &p2);
    }

    msg++; // convert WM_xBUTTONUP to WM_xBUTTONDBLCLK
  }

  SWELL_SendMouseMessage(hwnd2, msg, 0, MAKELPARAM(p2.x, p2.y), (int)b->x_root, (int)b->y_root);
  if (hwnd2) hwnd2->Release();
}


static bool is_our_oswindow(GdkWindow *w)
{
  while (w)
  {
    HWND hwnd = swell_oswindow_to_hwnd(w);
    if (hwnd) return true;
    w = gdk_window_get_effective_parent(w);
  }
  return false;

}

static void deactivateTimer(HWND hwnd, UINT uMsg, UINT_PTR tm, DWORD dwt)
{
  KillTimer(NULL,s_deactivate_timer);
  s_deactivate_timer=0;
  if (swell_app_is_inactive) return;
  GdkWindow *window = gdk_screen_get_active_window(gdk_screen_get_default());
  if (!is_our_oswindow(window))
    on_deactivate();

  update_menubar_activations();
}

extern SWELL_OSWINDOW swell_ignore_focus_oswindow;
extern DWORD swell_ignore_focus_oswindow_until;

void swell_gdk_drag_enter(int xpos, int ypos)
{
  if (!SWELL_DDrop_onDragEnter) return;

  const char *fn = swell_dragsrc_fn ? swell_dragsrc_fn : "/tmp/<<<<<unknown>>>>>>>";
  HGLOBAL h=GlobalAlloc(0,sizeof(DROPFILES) + strlen(fn) + 2);
  if (WDL_NORMALLY(h))
  {
    DROPFILES *hdr = (DROPFILES *)GlobalLock(h);
    if (WDL_NORMALLY(hdr))
    {
      memset(hdr,0,sizeof(*hdr));
      hdr->pFiles = sizeof(DROPFILES);
      hdr->pt.x = xpos;
      hdr->pt.y = ypos;
      char *ep = ((char *)hdr) + sizeof(*hdr);
      memcpy(ep, fn, strlen(fn)+1);
      ep[strlen(fn)+1] = 0;
      GlobalUnlock(h);
    }
    POINT pt = {xpos, ypos};
    SWELL_DDrop_onDragEnter(h,pt);
    GlobalFree(h);
  }
}


void swell_gdk_prepare_key_event()
{
  swell_dlg_destroyspare();
#ifdef SWELL_SUPPORT_GTK
  swell_im_update_candidates_location();
#endif
}

void swell_gdk_dispatch_event(GdkEvent *evt)
{
#ifdef SWELL_SUPPORT_GTK
  const bool swell_delete = evt->type == GDK_DELETE && swell_oswindow_to_hwnd(evt->any.window);
#endif

  switch (evt->type)
  {
    case GDK_FOCUS_CHANGE:
        {
          bool do_menus = false;
          GdkEventFocus *fc = (GdkEventFocus *)evt;
          if (s_deactivate_timer)
          {
            KillTimer(NULL,s_deactivate_timer);
            s_deactivate_timer=0;
            do_menus = true;
          }
          if (fc->in && is_our_oswindow(fc->window))
          {
            s_last_focus_change_time = GetTickCount();
            swell_on_toplevel_raise(fc->window);
            if (swell_ignore_focus_oswindow != fc->window ||
                (GetTickCount()-swell_ignore_focus_oswindow_until) < 0x10000000)
            {
              if (SWELL_focused_oswindow && SWELL_focused_oswindow != fc->window &&
                  swell_gdk_backend->invalidate_focus)
                swell_gdk_backend->invalidate_focus(SWELL_focused_oswindow);
              SWELL_focused_oswindow = fc->window;
              update_menubar_activations();
            }
            if (swell_app_is_inactive)
            {
              on_activate(0);
            }
          }
          else if (!swell_app_is_inactive)
          {
            s_deactivate_timer = SetTimer(NULL,0,200,deactivateTimer);
            if (gdk_options & OPTION_ALLOW_MAYBE_INACTIVE)
              do_menus = true;
          }
          if (do_menus)
            update_menubar_activations();
        }
    break;

    case GDK_DELETE:
     {
       HWND hwnd = swell_oswindow_to_hwnd(((GdkEventAny*)evt)->window);
       swell_gdk_close(hwnd);
     }
    break;
    case GDK_WINDOW_STATE: /// GdkEventWindowState for min/max
      OnWindowStateEvent((GdkEventWindowState*)evt);
    break;
    case GDK_GRAB_BROKEN:
      if (swell_oswindow_to_hwnd(((GdkEventAny*)evt)->window))
      {
        if (swell_captured_window)
        {
          SendMessage(swell_captured_window,WM_CAPTURECHANGED,0,0);
          swell_captured_window=0;
        }
      }
    break;
#ifdef GDK_AVAILABLE_IN_3_4
    case GDK_TOUCH_BEGIN:
    case GDK_TOUCH_UPDATE:
    case GDK_TOUCH_END:
    case GDK_TOUCH_CANCEL:
      {
        GdkEventTouch *e = (GdkEventTouch *)evt;
        static guint32 touchptr_lasttime; 
        bool doubletap = false;
        if (evt->type == GDK_TOUCH_BEGIN && !g_swell_touchptr)
        {
          DWORD now = e->time;
          doubletap = touchptr_lasttime && 
                      now >= touchptr_lasttime && 
                      now < touchptr_lasttime+350;
          touchptr_lasttime = now;
          g_swell_touchptr = e->sequence;
          g_swell_touchptr_wnd = e->window;
        }

        if (!e->sequence || e->sequence != g_swell_touchptr) 
        {
          touchptr_lasttime=0;
          break;
        }

        if (e->type == GDK_TOUCH_UPDATE)
        {
          GdkEventMotion m;
          memset(&m,0,sizeof(m));
          m.type = GDK_MOTION_NOTIFY;
          m.window = e->window;
          m.time = e->time;
          m.x = e->x;
          m.y = e->y;
          m.axes = e->axes;
          m.state = e->state;
          m.device = e->device;
          m.x_root = e->x_root;
          m.y_root = e->y_root;
          swell_gdk_backend->mouse_event((GdkEvent *)&m);
        }
        else
        {
          GdkEventButton but;
          memset(&but,0,sizeof(but));
          if (e->type == GDK_TOUCH_BEGIN) 
          {
            but.type = doubletap ? GDK_2BUTTON_PRESS:GDK_BUTTON_PRESS;
          }
          else 
          {
            but.type = GDK_BUTTON_RELEASE;
            g_swell_touchptr = NULL;
          }
          but.window = e->window;
          but.time = e->time;
          but.x = e->x;
          but.y = e->y;
          but.axes = e->axes;
          but.state = e->state;
          but.device = e->device;
          but.button = 1;
          but.x_root = e->x_root;
          but.y_root = e->y_root;
          swell_dlg_destroyspare();
          swell_gdk_backend->mouse_event((GdkEvent *)&but);
        } 
      }
    break;
#endif
    case GDK_MOTION_NOTIFY:
      gdk_event_request_motions((GdkEventMotion *)evt);
      swell_gdk_backend->mouse_event(evt);
    break;
    case GDK_SCROLL:
      swell_gdk_backend->mouse_event(evt);
    break;
    case GDK_BUTTON_PRESS:
    case GDK_2BUTTON_PRESS:
    case GDK_BUTTON_RELEASE:
      swell_dlg_destroyspare();
      swell_gdk_backend->mouse_event(evt);
    break;
    default:
          //printf("msg: %d\n",evt->type);
    break;
  }
#ifdef SWELL_SUPPORT_GTK
  if (!swell_delete && swell_gtk_main_do_event)
    swell_gtk_main_do_event(evt);
#endif
}

static void swell_gdkEventHandler(GdkEvent *evt, gpointer)
{
  GdkEvent *old = swell_gdk_current_event;
  swell_gdk_current_event = evt;
  swell_gdk_backend->event(evt);
  swell_gdk_current_event = old;
}


void swell_oswindow_begin_drag(HWND source)
{
  if (swell_gdk_backend->begin_drag) swell_gdk_backend->begin_drag(source);
}

bool swell_oswindow_from_point(POINT point, HWND *window)
{
  if (!swell_gdk_backend->window_from_point) return false;
  *window = swell_gdk_backend->window_from_point(point);
  return true;
}

bool swell_oswindow_can_run_timers()
{
  return !swell_gdk_backend->can_run_timers || swell_gdk_backend->can_run_timers();
}

bool swell_oswindow_reuse_popup()
{
  return swell_gdk_backend->reuse_popup;
}

void swell_oswindow_end_modal(HWND hwnd)
{
  if (swell_gdk_backend->end_modal) swell_gdk_backend->end_modal(hwnd);
}

void SWELL_RunEvents()
{
  if (SWELL_gdk_active > 0) swell_gdk_backend->run_events();
}

bool swell_gdk_set_fullscreen(HWND hwnd, int fs) // fs=2 if window might have owned children
{
  if (!hwnd) return false;
  if (fs==2 && (gdk_options & OPTION_FULLSCREEN_FOR_OWNER_WINDOWS)!=OPTION_FULLSCREEN_FOR_OWNER_WINDOWS) return false;

  const int stylemask = WS_BORDER|WS_CAPTION|WS_THICKFRAME;
  if (!hwnd->m_oswindow || !(gdk_options & OPTION_FULLSCREEN_DYNAMIC))
  {
    hwnd->m_oswindow_fullscreen = fs ? WS_VISIBLE | (hwnd->m_style & stylemask) : 0;
    return true; // request caller hide/show
  }

  // OPTION_FULLSCREEN_DYNAMIC and window exists, modify state
  if (fs)
  {
    hwnd->m_oswindow_fullscreen = WS_VISIBLE | (hwnd->m_style & stylemask);
    hwnd->m_style &= ~stylemask;
    gdk_window_fullscreen(hwnd->m_oswindow);
  }
  else
  {
    hwnd->m_style |= (hwnd->m_oswindow_fullscreen & stylemask);
    hwnd->m_oswindow_fullscreen = 0;
    gdk_window_unfullscreen(hwnd->m_oswindow);
  }
  return false;
}

void swell_gdk_set_decorations(HWND hwnd)
{
  if (hwnd->m_style & WS_CAPTION)
  {
    if (hwnd->m_style & WS_THICKFRAME)
      gdk_window_set_decorations(hwnd->m_oswindow,(GdkWMDecoration) (GDK_DECOR_ALL | GDK_DECOR_MENU));
    else
      gdk_window_set_decorations(hwnd->m_oswindow,(GdkWMDecoration) (GDK_DECOR_BORDER|GDK_DECOR_TITLE|GDK_DECOR_MINIMIZE));
  }
  else
  {
    gdk_window_set_decorations(hwnd->m_oswindow,(GdkWMDecoration) 0);
  }
}

void swell_oswindow_update_style(HWND hwnd, LONG oldstyle)
{
  const LONG val = hwnd->m_style, ret = oldstyle;
  if (hwnd->m_oswindow && ((ret^val)& WS_CAPTION))
  {
    swell_hide_owned_windows_transient(hwnd);

    gdk_window_hide(hwnd->m_oswindow);
    swell_gdk_backend->set_decorations(hwnd);
    hwnd->m_oswindow_private |= PRIVATE_NEEDSHOW;
  }
}

void swell_oswindow_update_enable(HWND hwnd)
{
  if (hwnd->m_oswindow && !swell_app_is_inactive) 
    gdk_window_set_accept_focus(hwnd->m_oswindow,hwnd->m_enabled);
}

int SWELL_SetWindowLevel(HWND hwnd, int newlevel)
{
  int rv=0;
  if (hwnd)
  {
    rv = hwnd->m_israised ? 1 : 0;
    hwnd->m_israised = newlevel>0;
    if (hwnd->m_oswindow) gdk_window_set_keep_above(hwnd->m_oswindow,newlevel>0 && !swell_app_is_inactive);
  }
  return rv;
}

void swell_gdk_monitor_geometry(int index, bool work, GdkRectangle *rect)
{
  GdkScreen *screen = gdk_screen_get_default();
#if SWELL_TARGET_GDK != 2
  if (work) gdk_screen_get_monitor_workarea(screen, index, rect);
  else
#endif
    gdk_screen_get_monitor_geometry(screen, index, rect);
}

void SWELL_GetViewPort(RECT *r, const RECT *sourcerect, bool wantWork)
{
  r->left=r->top=0;
  r->right=1024;
  r->bottom=768;
  if (!swell_initwindowsys()) return;
  GdkScreen *defscr = gdk_screen_get_default();
  if (!defscr) return;
  const gint n = gdk_screen_get_n_monitors(defscr);
  if (n < 1) return;

  const gint prim = gdk_screen_get_primary_monitor(defscr);
  double best_score = -1e20;
  RECT sr;
  if (sourcerect) sr = *sourcerect;
  if (swell_gdk_backend->get_viewport && swell_gdk_backend->get_viewport(r)) return;
  for (gint idx = 0; idx < n; idx ++)
  {
    GdkRectangle rc={0,0,1024,1024};
    if (!sourcerect && prim>0) idx = prim;

    swell_gdk_backend->monitor_geometry(idx, wantWork, &rc);
    RECT tmp;
    tmp.left=rc.x;
    tmp.top = rc.y;
    tmp.right=rc.x+rc.width;
    tmp.bottom=rc.y+rc.height;
    if (!sourcerect || n < 2)
    {
      *r = tmp;
      break;
    }

    double score;
    RECT res;
    if (IntersectRect(&res, &tmp, &sr))
    {
      score = wdl_abs((res.right-res.left) * (res.bottom-res.top));
    }
    else
    {
      int dx = 0, dy = 0;
      if (tmp.left > sr.right) dx = tmp.left - sr.right;
      else if (tmp.right < sr.left) dx = sr.left - tmp.right;
      if (tmp.bottom < sr.top) dy = tmp.bottom - sr.top;
      else if (tmp.top > sr.bottom) dy = tmp.top - sr.bottom;
      score = - (dx*dx + dy*dy);
    }

    if (!idx || score > best_score)
    {
      best_score = score;
      *r = tmp;
    }
  }
}


bool GetWindowRect(HWND hwnd, RECT *r)
{
  if (!hwnd) return false;
  if (hwnd->m_oswindow)
  {
    swell_gdk_backend->get_window_rect(hwnd, r);
    return true;
  }

  r->left=r->top=0; 
  ClientToScreen(hwnd,(LPPOINT)r);
  r->right = r->left + hwnd->m_position.right - hwnd->m_position.left;
  r->bottom = r->top + hwnd->m_position.bottom - hwnd->m_position.top;
  return true;
}

void swell_oswindow_begin_resize(SWELL_OSWINDOW wnd)
{
  // make sure window is resizable (hints will be re-set on upcoming CONFIGURE event)
  GdkGeometry geom = {0};
  gdk_window_set_geometry_hints(wnd, &geom, (GdkWindowHints)0);
}

void swell_oswindow_resize(SWELL_OSWINDOW wnd, int reposflag, RECT rect, bool mapped)
{
  swell_gdk_backend->resize_window(wnd, reposflag, rect, mapped);
}


void swell_oswindow_postresize(HWND hwnd, RECT f) 
{ 
  if (hwnd->m_oswindow && (hwnd->m_oswindow_private&PRIVATE_NEEDSHOW) && !hwnd->m_oswindow_fullscreen)
  {
    gdk_window_show(hwnd->m_oswindow);
    if (hwnd->m_style & WS_CAPTION) gdk_window_unmaximize(hwnd->m_oswindow); // fixes Kwin
    swell_oswindow_resize(hwnd->m_oswindow,3,f); // fixes xfce
    hwnd->m_oswindow_private &= ~PRIVATE_NEEDSHOW;

    swell_set_owned_windows_transient(hwnd,false);
  }
  if (hwnd->m_oswindow && !(hwnd->m_style&WS_THICKFRAME))
  {
    // kwin requires this for non-sizeable windows (doing it in the configure event is too late, apparently)
    swell_gdk_backend->set_size_hints(hwnd, &f);
  }
}

void UpdateWindow(HWND hwnd)
{
#if SWELL_TARGET_GDK == 2
  if (hwnd)
  {
    while (hwnd && !hwnd->m_oswindow) hwnd=hwnd->m_parent;
    if (hwnd && hwnd->m_oswindow) gdk_window_process_updates(hwnd->m_oswindow,true);
  }
#endif
}

void swell_oswindow_invalidate(HWND hwnd, const RECT *rect)
{
  swell_gdk_backend->invalidate(hwnd, rect);
}


void GetCursorPos(POINT *pt)
{
  pt->x = pt->y = 0;
  if (SWELL_gdk_active > 0) swell_gdk_backend->get_cursor_pos(pt);
}


WORD swell_gdk_get_async_key_state(int key)
{
  if (SWELL_gdk_active>0)
  {
    GdkModifierType mod=(GdkModifierType)0;
    HWND h = GetFocus();
    while (h && !h->m_oswindow) h = h->m_parent;
//#if SWELL_TARGET_GDK == 3
//    GdkDevice *dev=NULL;
//    if (swell_gdk_current_event) dev = gdk_event_get_device(swell_gdk_current_event);
//    if (!dev) dev = gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdk_display_get_default()));
//    if (dev) gdk_window_get_device_position(h?  h->m_oswindow : gdk_get_default_root_window(),dev, NULL, NULL,&mod);
//#else
    gdk_window_get_pointer(h?  h->m_oswindow : gdk_get_default_root_window(),NULL,NULL,&mod);
//#endif
 
    if (key == VK_LBUTTON) return ((mod&GDK_BUTTON1_MASK)||g_swell_touchptr)?0x8000:0;
    if (key == VK_MBUTTON) return (mod&GDK_BUTTON2_MASK)?0x8000:0;
    if (key == VK_RBUTTON) return (mod&GDK_BUTTON3_MASK)?0x8000:0;

    if (key == VK_CONTROL) return (mod&GDK_CONTROL_MASK)?0x8000:0;
    if (key == VK_MENU) return (mod&GDK_MOD1_MASK)?0x8000:0;
    if (key == VK_SHIFT) return (mod&GDK_SHIFT_MASK)?0x8000:0;
    if (key == VK_LWIN) return (mod&SWELL_WINDOWSKEY_GDK_MASK)?0x8000:0;
  }
  return 0;
}

WORD GetAsyncKeyState(int key)
{
  return swell_gdk_backend->get_async_key_state(key);
}

DWORD GetMessagePos()
{  
  return swell_lastMessagePos;
}

HWND SWELL_CreateXBridgeWindow(HWND parent, void **window, const RECT *rect)
{
  return swell_gdk_backend->create_bridge(parent, window, rect);
}


static void initiate_drag(HWND hwnd, const SWELL_GdkDrag &data)
{
  if (hwnd) hwnd->Retain();
  const bool is_osw_cursor = s_last_setcursor_oswnd && swell_oswindow_from_hwnd(hwnd) == s_last_setcursor_oswnd;
  swell_dragsrc_fn = data.count > 0 ? data.files[0] : data.filename;
  swell_gdk_backend->drag_files(hwnd, data);
  if (is_osw_cursor && hwnd && !hwnd->m_hashaddestroy && GetFocus() != hwnd)
  {
    GdkWindow *window = swell_oswindow_from_hwnd(hwnd);
    if (window) gdk_window_set_cursor(window, NULL);
  }
  swell_dragsrc_fn = NULL;
  if (hwnd) hwnd->Release();
}

void SWELL_InitiateDragDrop(HWND hwnd, RECT *srcrect, const char *srcfn, void (*callback)(const char *))
{
  WDL_String filename(srcfn);
  SWELL_GdkDrag data = { NULL, 0, filename.Get(), callback };
  initiate_drag(hwnd, data);
}

// owner owns srclist, make copies here etc
void SWELL_InitiateDragDropOfFileList(HWND hwnd, RECT *srcrect, const char **srclist, int srccount, HICON icon)
{
  SWELL_GdkDrag data = { srclist, srccount, NULL, NULL };
  initiate_drag(hwnd, data);
}

void SWELL_FinishDragDrop() { }


bool SWELL_IsCursorVisible()
{
  return s_cursor_vis_cnt>=0;
}


void SWELL_SetCursor(HCURSOR curs)
{
  if (s_last_setcursor == curs && SWELL_focused_oswindow == s_last_setcursor_oswnd) return;

  s_last_setcursor=curs;
  s_last_setcursor_oswnd = SWELL_focused_oswindow;
  if (SWELL_focused_oswindow)
  {
    swell_gdk_backend->set_cursor(SWELL_focused_oswindow, (GdkCursor*)curs,
                                  GetCapture() && !(gdk_options & OPTION_IS_WAYLAND));
  }
}

HCURSOR SWELL_GetCursor()
{
  return s_last_setcursor;
}
HCURSOR SWELL_GetLastSetCursor()
{
  return s_last_setcursor;
}

int SWELL_ShowCursor(BOOL bShow)
{
  s_cursor_vis_cnt += (bShow?1:-1);
  if (s_cursor_vis_cnt==-1 && !bShow) 
  {
    gint x1, y1;
    #if SWELL_TARGET_GDK == 3
    GdkDevice *dev = gdk_device_manager_get_client_pointer (gdk_display_get_device_manager (gdk_display_get_default ()));
    gdk_device_get_position (dev, NULL, &x1, &y1);
    #else
    gdk_display_get_pointer(gdk_display_get_default(), NULL, &x1, &y1, NULL);
    #endif
    g_swell_mouse_relmode_curpos_x = x1;
    g_swell_mouse_relmode_curpos_y = y1;
    s_last_cursor = GetCursor();
    SetCursor((HCURSOR)gdk_cursor_new_for_display(gdk_display_get_default(),GDK_BLANK_CURSOR));
    g_swell_mouse_relmode = swell_gdk_backend->relative_mouse;
  }
  if (s_cursor_vis_cnt==0 && bShow) 
  {
    SetCursor(s_last_cursor);
    g_swell_mouse_relmode=false;
    if (!g_swell_touchptr)
    {
      #if SWELL_TARGET_GDK == 3
      gdk_device_warp(gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdk_display_get_default())),
                     gdk_screen_get_default(),
                     g_swell_mouse_relmode_curpos_x, g_swell_mouse_relmode_curpos_y);
      #else
      gdk_display_warp_pointer(gdk_display_get_default(),gdk_screen_get_default(), g_swell_mouse_relmode_curpos_x, g_swell_mouse_relmode_curpos_y);
      #endif
    }
  }
  return s_cursor_vis_cnt;
}

BOOL SWELL_SetCursorPos(int X, int Y)
{  
  if (g_swell_mouse_relmode || g_swell_touchptr) return false;
 
  #if SWELL_TARGET_GDK == 3
  gdk_device_warp(gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdk_display_get_default())),
                     gdk_screen_get_default(),
                     X, Y);
  #else
  gdk_display_warp_pointer(gdk_display_get_default(),gdk_screen_get_default(), X, Y);
  #endif
  return true;
}

static void getHotSpotForFile(const char *fn, POINT *pt)
{
  FILE *fp = WDL_fopenA(fn,"rb");
  if (!fp) return;
  unsigned char buf[32];
  if (fread(buf,1,6,fp)==6 && !buf[0] && !buf[1] && buf[2] == 2 && buf[3] == 0 && buf[4] == 1 && buf[5] == 0)
  {
    if (fread(buf,1,16,fp)==16)
    {
      pt->x = buf[4]|(buf[5]<<8);
      pt->y = buf[6]|(buf[7]<<8);
    }
  }
  fclose(fp);
}

HCURSOR SWELL_LoadCursorFromFile(const char *fn)
{
  GdkPixbuf *pb = gdk_pixbuf_new_from_file(fn,NULL);
  if (pb) 
  {
    POINT hs = {0,};
    getHotSpotForFile(fn,&hs);
    GdkCursor *curs = gdk_cursor_new_from_pixbuf(gdk_display_get_default(),pb,hs.x,hs.y);
    g_object_unref(pb);
    return (HCURSOR) curs;
  }
  return NULL;
}

HCURSOR SWELL_LoadCursor(const char *_idx)
{
  GdkCursorType def = GDK_LEFT_PTR;
  if (_idx == IDC_NO) def = GDK_PIRATE;
  else if (_idx == IDC_SIZENWSE) def = GDK_BOTTOM_LEFT_CORNER;
  else if (_idx == IDC_SIZENESW) def = GDK_BOTTOM_RIGHT_CORNER;
  else if (_idx == IDC_SIZEALL) def = GDK_FLEUR;
  else if (_idx == IDC_SIZEWE) def =  GDK_RIGHT_SIDE;
  else if (_idx == IDC_SIZENS) def = GDK_TOP_SIDE;
  else if (_idx == IDC_ARROW) def = GDK_LEFT_PTR;
  else if (_idx == IDC_HAND) def = GDK_HAND1;
  else if (_idx == IDC_UPARROW) def = GDK_CENTER_PTR;
  else if (_idx == IDC_IBEAM) def = GDK_XTERM;
  else 
  {
    SWELL_CursorResourceIndex *p = SWELL_curmodule_cursorresource_head;
    while (p)
    {
      if (p->resid == _idx)
      {
        if (p->cachedCursor) return p->cachedCursor;
        // todo: load from p->resname, into p->cachedCursor, p->hotspot
        char buf[1024];
        GetModuleFileName(NULL,buf,sizeof(buf));
        WDL_remove_filepart(buf);
        snprintf_append(buf,sizeof(buf),"/Resources/%s.cur",p->resname);
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(buf,NULL);
        if (pb) 
        {
          getHotSpotForFile(buf,&p->hotspot);
          GdkCursor *curs = gdk_cursor_new_from_pixbuf(gdk_display_get_default(),pb,p->hotspot.x,p->hotspot.y);
          g_object_unref(pb);
          return (p->cachedCursor = (HCURSOR) curs);
        }
      }
      p=p->_next;
    }
  }

  HCURSOR hc= (HCURSOR)gdk_cursor_new_for_display(gdk_display_get_default(),def);

  return hc;
}

void SWELL_Register_Cursor_Resource(const char *idx, const char *name, int hotspot_x, int hotspot_y)
{
  SWELL_CursorResourceIndex *ri = (SWELL_CursorResourceIndex*)malloc(sizeof(SWELL_CursorResourceIndex));
  ri->hotspot.x = hotspot_x;
  ri->hotspot.y = hotspot_y;
  ri->resname=name;
  ri->cachedCursor=0;
  ri->resid = idx;
  ri->_next = SWELL_curmodule_cursorresource_head;
  SWELL_curmodule_cursorresource_head = ri;
}

int SWELL_KeyToASCII(int wParam, int lParam, int *newflags)
{
  return 0;
}

void swell_scaling_init(bool no_auto_hidpi)
{
  swell_gdk_backend->scaling_init(no_auto_hidpi);
}

BOOL EnumDisplayMonitors(HDC hdc,const LPRECT r,MONITORENUMPROC proc,LPARAM lParam)
{
  GdkScreen *defscr = gdk_screen_get_default();
  const int nmon = gdk_screen_get_n_monitors(defscr);
  for (int x = 0; x < nmon; x ++)
  {
    GdkRectangle rc={0,0,1024,1024};
    swell_gdk_backend->monitor_geometry(x, false, &rc);
    RECT screen_rect, tmp = { rc.x, rc.y,rc.x+rc.width , rc.y+rc.height };
    if (r)
    {
      if (!IntersectRect(&screen_rect,r,&tmp))
        continue;
    }
    else
    {
      screen_rect = tmp;
    }

    if (!proc((HMONITOR)(INT_PTR) (x+1),hdc,&screen_rect,lParam)) break;
  }
  return TRUE;
}
BOOL GetMonitorInfo(HMONITOR hmon, void *inf)
{
  GdkScreen *defscr = gdk_screen_get_default();
  const int nmon = gdk_screen_get_n_monitors(defscr);
  const int monidx = ((int) (INT_PTR) hmon)-1;
  if (monidx<0 || monidx >= nmon) return FALSE;

  MONITORINFOEX *a = (MONITORINFOEX*)inf;
  if (a->cbSize < sizeof(MONITORINFO)) return FALSE;
  a->dwFlags = 0;
  GdkRectangle rc={0,0,1024,1024};
  swell_gdk_backend->monitor_geometry(monidx, false, &rc);
  RECT tmp = { rc.x, rc.y,rc.x+rc.width , rc.y+rc.height };
  a->rcMonitor = a->rcWork = tmp;

  if (a->cbSize > sizeof(MONITORINFO))
  {
    const int maxlen = (int) (a->cbSize - sizeof(MONITORINFO));
    const char *s = gdk_screen_get_monitor_plug_name(defscr,monidx);
    if (!s) return FALSE;
    lstrcpyn_safe(a->szDevice,s,maxlen);
  }

  return TRUE;
}


void *SWELL_GetOSWindow(HWND hwnd, const char *type)
{
  if (hwnd && !strcmp(type,"GdkWindow")) return hwnd->m_oswindow;
  return NULL;
}

void *SWELL_GetOSEvent(const char *type)
{
  return !strcmp(type,"GdkEvent") ? swell_gdk_current_event : NULL;
}

void swell_gdk_prevent_screensaver(bool prev, const char *desc)
{
  static GDBusProxy *s_ss_prox;
  static char s_ss_mode;
  enum { SS_MODE_FD=0, SS_MODE_XFCE, NUM_SS_MODES } ;

  if (s_ss_mode < NUM_SS_MODES && !!s_ss_prox != prev)
  {
    static unsigned int s_ss_cookie;
again:
    const char *ident = s_ss_mode==SS_MODE_XFCE ? "org.xfce.ScreenSaver" : "org.freedesktop.ScreenSaver";
    if (!s_ss_prox && prev)
    {
      s_ss_prox = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
          G_DBUS_PROXY_FLAGS_NONE, NULL,
          ident,
          s_ss_mode==SS_MODE_XFCE ? "/org/xfce/ScreenSaver" : "/org/freedesktop/ScreenSaver",
          ident,
          NULL, NULL);
      if (!s_ss_prox && ++s_ss_mode < NUM_SS_MODES) goto again; // g_dbus_proxy_new_for_bus_sync() should always succeed, though, even if not supported interface
    }

    if (s_ss_prox)
    {
      GVariant *result = g_dbus_proxy_call_sync(s_ss_prox,
                            prev ? "Inhibit" : "UnInhibit",
                            prev ? g_variant_new("(ss)", desc?desc:"swell app", "running") : g_variant_new("(u)", s_ss_cookie),
                            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);

      if (prev)
      {
        if (!result)
        {
#ifdef _DEBUG
          printf("swell-generic-gdk: dbus call of %s.Inhibit failed\n", ident);
#endif
          if (++s_ss_mode < NUM_SS_MODES)
          {
            g_object_unref(s_ss_prox);
            s_ss_prox = NULL;
            goto again;
          }
        }
        else
        {
          g_variant_get(result, "(u)", &s_ss_cookie);
        }
      }
      else
      {
        s_ss_cookie = 0;
        g_object_unref(s_ss_prox);
        s_ss_prox = NULL;
      }

      if (result) g_variant_unref(result);
    }
  }
}


#endif
#endif
