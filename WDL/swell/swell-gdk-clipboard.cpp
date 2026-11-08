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
#include "swell.h"
#include "swell-internal.h"
#include "swell-gdk.h"
#include "../wdlcstring.h"

SWELL_GdkClipboard swell_gdk_clipboard;

//// clipboard state

static WDL_PtrSet<UINT> s_clipboard_enumstate; // populated on first EnumClipboardFormats() call, or by SetClipboardData()

struct clip_fmt {
  const char *str;
  GdkAtom atom;
};
static WDL_TypedBuf<clip_fmt> s_clip_fmts; // mapping of UINT clipboard types, offset by 1 (s_clip_fmts.Get()[0] is CF_TEXT=1, etc)


static bool s_clipboard_written; // has clipboard data been written-to since opening

GdkAtom swell_gdk_utf8_atom()
{
  static GdkAtom tmp;
  if (!tmp) tmp = gdk_atom_intern_static_string("UTF8_STRING");
  return tmp;
}

GdkAtom swell_gdk_targets_atom()
{
  static GdkAtom tmp;
  if (!tmp) tmp = gdk_atom_intern_static_string("TARGETS");
  return tmp;
}

GdkAtom swell_gdk_atom_atom()
{
  static GdkAtom tmp;
  if (!tmp) tmp = gdk_atom_intern_static_string("ATOM");
  return tmp;
}

GdkAtom swell_gdk_urilist_atom()
{
  static GdkAtom tmp;
  if (!tmp) tmp = gdk_atom_intern_static_string("text/uri-list");
  return tmp;
}

static bool atom_from_clipboard_type(int type, GdkAtom *atom)
{
  type--;
  if (type < 0 || type >= s_clip_fmts.GetSize()) return false;
  *atom = s_clip_fmts.Get()[type].atom;
  return true;
}

static UINT clipboard_type_from_atom(GdkAtom atom)
{
  if (swell_gdk_backend->clipboard_text && swell_gdk_backend->clipboard_text(atom)) return CF_TEXT;
  for (int x = 0; x < s_clip_fmts.GetSize(); x++)
    if (s_clip_fmts.Get()[x].atom == atom) return x+1;
  return 0;
}

HANDLE *swell_gdk_clipboard_find(GdkAtom a)
{
  for (int x = 0; x < swell_gdk_clipboard.formats.GetSize(); x ++)
    if (swell_gdk_clipboard.formats.Get()[x] == a)
      return swell_gdk_clipboard.contents.Get() + x;
  return NULL;
}

void swell_gdk_encode_uri(WDL_FastString *s, const char *rd)
{
  while (*rd)
  {
    // unsure if UTF-8 chars should be urlencoded or allowed?
    if (*rd < 0 || (!isalnum_safe(*rd) && *rd != '-' && *rd != '_' && *rd != '.' && *rd != '/'))
    {
      char buf[8];
      snprintf(buf,sizeof(buf),"%%%02x",(int)(unsigned char)*rd);
      s->Append(buf);
    }
    else s->Append(rd,1);

    rd++;
  }
}

void swell_gdk_clipboard_format(GdkAtom format, HANDLE handle, WDL_FastString &str)
{
  const char *data = (const char*)handle;
  const int len = GlobalSize(handle);
  if (format == swell_gdk_utf8_atom())
  {
    for (int i = 0; i < len && data[i]; ++i)
    {
      if (data[i] == '\r' && i + 1 < len && data[i+1] == '\n') continue;
      str.Append(data + i, 1);
    }
  }
  else if (format == swell_gdk_urilist_atom())
  {
    if (len <= (int)sizeof(DROPFILES)) return;
    const DROPFILES *hdr = (const DROPFILES*)data;
    if (hdr->fWide || hdr->pFiles >= (DWORD)len) return; // todo deal with UTF-16
    for (const char *file = data + hdr->pFiles; file < data + len && *file; )
    {
      const char *end = (const char*)memchr(file, 0, data + len - file);
      if (!end) break;
      str.Append("file://");
      swell_gdk_encode_uri(&str, file);
      str.Append("\r\n");
      file = end + 1;
    }
  }
  else str.SetRaw(data, len);
}

HANDLE swell_gdk_urilist_to_dropfiles(const POINT *pt, const guchar *gptr, gint sz)
{
  if (!gptr || sz <= 0) return NULL;
  gchar *text = g_strndup((const gchar*)gptr, sz);
  gchar **uris = g_uri_list_extract_uris(text);
  g_free(text);
  GString *files = g_string_new(NULL);
  for (gchar **uri = uris; *uri; ++uri)
  {
    gchar *hostname = NULL;
    gchar *path = g_filename_from_uri(*uri, &hostname, NULL);
    if (path && (!hostname || !g_ascii_strcasecmp(hostname, "localhost") ||
                 !g_ascii_strcasecmp(hostname, g_get_host_name())))
      g_string_append_len(files, path, strlen(path) + 1);
    g_free(path);
    g_free(hostname);
  }
  g_strfreev(uris);

  HANDLE gobj = NULL;
  if (files->len && files->len <= INT_MAX - sizeof(DROPFILES) - 1)
    gobj = GlobalAlloc(0, sizeof(DROPFILES) + files->len + 1);
  if (gobj)
  {
    DROPFILES *df = (DROPFILES*)gobj;
    memset(df, 0, sizeof(*df));
    df->pFiles = sizeof(DROPFILES);
    if (pt) df->pt = *pt;
    memcpy(df + 1, files->str, files->len + 1);
  }
  g_string_free(files, TRUE);
  return gobj;
}

bool OpenClipboard(HWND hwndDlg)
{
  RegisterClipboardFormat(NULL);
  swell_gdk_clipboard.owner=hwndDlg ? hwndDlg : SWELL_topwindows;
  s_clipboard_written = false;
  if (swell_gdk_backend->clipboard_open) swell_gdk_backend->clipboard_open();

  GlobalFree(swell_gdk_clipboard.read_data);
  swell_gdk_clipboard.read_data = NULL;
  swell_gdk_clipboard.read_type = NULL;

  s_clipboard_enumstate.DeleteAll();

  return true;
}

void CloseClipboard()
{
  if (s_clipboard_written && swell_gdk_backend->clipboard_publish)
    swell_gdk_backend->clipboard_publish();
  s_clipboard_written = false;
  s_clipboard_enumstate.DeleteAll();
  swell_gdk_clipboard.owner=NULL;
}

UINT EnumClipboardFormats(UINT lastfmt)
{
  if (lastfmt == 0 && !s_clipboard_written && !s_clipboard_enumstate.GetSize())
  {
    HANDLE h = swell_gdk_backend->clipboard_read(swell_gdk_targets_atom());
    if (!h) return 0;
    int sz = GlobalSize(h) / (int)sizeof(GdkAtom);
    const char *l = (const char *)h;
    for (int x = 0; x < sz; x ++)
    {
      GdkAtom a;
      memcpy(&a,l + x*sizeof(GdkAtom), sizeof(a));
      UINT c = clipboard_type_from_atom(a);
      if (c) s_clipboard_enumstate.Insert(c);
    }
  }

  int x = 0;
  UINT fmt;
  if (lastfmt) while (s_clipboard_enumstate.Enumerate(x++,&fmt) && fmt != lastfmt);
  return s_clipboard_enumstate.Enumerate(x,&fmt) ? fmt : 0;
}

HANDLE GetClipboardData(UINT type)
{
  RegisterClipboardFormat(NULL);

  GdkAtom a;
  if (atom_from_clipboard_type(type,&a))
  {
    if (s_clipboard_written && swell_gdk_backend->clipboard_publish)
    {
      HANDLE *state = swell_gdk_clipboard_find(a);
      return state ? *state : NULL;
    }
    return swell_gdk_backend->clipboard_read(a);
  }

  return NULL;
}

void EmptyClipboard()
{
  for (int x = 0; x < swell_gdk_clipboard.contents.GetSize(); x ++)
    GlobalFree(swell_gdk_clipboard.contents.Get()[x]);
  swell_gdk_clipboard.formats.Resize(0);
  swell_gdk_clipboard.contents.Resize(0);
  if (swell_gdk_backend->clipboard_publish)
  {
    s_clipboard_written = true;
    s_clipboard_enumstate.DeleteAll();
  }
}

void SetClipboardData(UINT type, HANDLE h)
{
  RegisterClipboardFormat(NULL);
  GdkAtom a;
  if (atom_from_clipboard_type(type,&a))
  {
    HANDLE *state = swell_gdk_clipboard_find(a);
    if (!state)
    {
      swell_gdk_clipboard.formats.Add(a);
      swell_gdk_clipboard.contents.Add(h);
    }
    else if (*state != h)
    {
      GlobalFree(*state);
      *state = h;
    }

    if (!s_clipboard_written &&
        (!swell_gdk_backend->clipboard_claim || swell_gdk_backend->clipboard_claim()))
    {
      s_clipboard_written = true;
      s_clipboard_enumstate.DeleteAll();
    }
    if (WDL_NORMALLY(type != 0))
      s_clipboard_enumstate.Insert(type);
  }
}

UINT RegisterClipboardFormat(const char *desc)
{
  if (!s_clip_fmts.GetSize())
  {
    clip_fmt c[2] = {
      { "SWELL__CF_TEXT", swell_gdk_utf8_atom() },
      { "SWELL__CF_HDROP", swell_gdk_urilist_atom() },
    };
    s_clip_fmts.Add(c,sizeof(c)/sizeof(c[0]));
  }

  if (!desc || !*desc) return 0;

  for (int x = 0; x < s_clip_fmts.GetSize(); x++)
    if (!strcmp(s_clip_fmts.Get()[x].str,desc)) return x + 1;

  char tmp[512];
  snprintf(tmp,sizeof(tmp),"application/swell-%s",desc);

  clip_fmt c1 = { strdup(desc), gdk_atom_intern(tmp, 0) };
  s_clip_fmts.Add(c1);

  return s_clip_fmts.GetSize();
}


#endif
