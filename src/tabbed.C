/*
 * tabbed.C - native tab support for rxvt-unicode
 *
 * One frame window with a tab bar; each tab is a full rxvt_term
 * embedded into the frame through the -embed/XEmbed mechanism.
 */

#include "../config.h"
#include "rxvt.h"
#include "tabbed.h"

#if ENABLE_XEMBED

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <X11/Xutil.h>

rxvt_tab_manager *tab_manager;

static const int CELL_W = 7;   // rough cell size for the default frame size
static const int CELL_H = 14;

rxvt_tab_manager::rxvt_tab_manager ()
: display (0), frame (0), active (-1), gc (0), font (0),
  tab_h (20), frame_w (0), frame_h (0),
  spawn_argv (0), spawn_argc (0), envv (0)
{
}

rxvt_tab_manager::~rxvt_tab_manager ()
{
  // unregister first: the display (and its event loop state) may go
  // away when we drop our reference below
  frame_ev.stop (display);

  for (int i = tabs.size (); i--; )
    {
      tabs[i]->ev.stop (display);
      delete tabs[i];
    }

  if (font) XFreeFont (display->dpy, font);
  if (gc) XFreeGC (display->dpy, gc);
  if (frame) XDestroyWindow (display->dpy, frame);

  if (display)
    {
      display->flush ();
      displays.put (display);
    }

  for (int i = 0; i < spawn_argc; i++)
    free (spawn_argv[i]);

  delete[] spawn_argv;
}

bool
rxvt_tabbed_requested (int argc, const char *const *argv)
{
  // tabs are always on, except when embedding into another window
  for (int i = 1; i < argc; i++)
    if (!strcmp (argv[i], "-embed"))
      return false;

  return true;
}

void
rxvt_tab_manager::run (int argc, const char *const *argv, const char *const *envv)
{
  this->envv = envv;

  // keep all arguments except the -tab switch itself
  spawn_argc = 0;
  spawn_argv = new char *[argc + 3];

  const char *geom = 0;

  for (int i = 0; i < argc; i++)
    {
      if (i && (!strcmp (argv[i], "-g") || !strcmp (argv[i], "-geometry")
                || !strcmp (argv[i], "--geometry"))
          && i + 1 < argc)
        geom = argv[++i];

      spawn_argv[spawn_argc++] = strdup (argv[i]);
    }

  if (!(display = displays.get (getenv ("DISPLAY"))))
    rxvt_fatal ("unable to open display, aborting.\n");

  int cols = 80, rows = 24, x = 0, y = 0;
  int geom_mask = geom ? XParseGeometry (geom, &x, &y, (unsigned *)&cols, (unsigned *)&rows) : 0;

  frame_w = cols * CELL_W + 14;
  frame_h = rows * CELL_H + 14 + tab_h;

  Display *dpy = display->dpy;

  font = XLoadQueryFont (dpy, "fixed");

  if (font)
    {
      tab_h = font->max_bounds.ascent + font->max_bounds.descent + 7;

      XGCValues gcv;
      gcv.font = font->fid;
      gcv.foreground = WhitePixel (dpy, DefaultScreen (dpy));
      gcv.background = BlackPixel (dpy, DefaultScreen (dpy));
      gcv.graphics_exposures = False;
      gc = XCreateGC (dpy, DefaultRootWindow (dpy),
                      GCFont | GCForeground | GCBackground | GCGraphicsExposures, &gcv);
    }

  XSetWindowAttributes sattr;
  sattr.event_mask = ExposureMask | StructureNotifyMask | FocusChangeMask
                     | ButtonPressMask | ButtonReleaseMask;
  sattr.background_pixel = BlackPixel (dpy, DefaultScreen (dpy));

  frame = XCreateWindow (dpy, DefaultRootWindow (dpy),
                         x, y, frame_w, frame_h, 0,
                         CopyFromParent, InputOutput, CopyFromParent,
                         CWEventMask | CWBackPixel, &sattr);

  XSizeHints sh;
  sh.flags = PResizeInc | PBaseSize | USSize;
  sh.width_inc = 1;
  sh.height_inc = 1;
  sh.base_width = 0;
  sh.base_height = tab_h;
  sh.width = frame_w;
  sh.height = frame_h;

  if (geom_mask & (XValue | YValue))
    {
      sh.flags |= USPosition;
      sh.x = x;
      sh.y = y;
    }

  XSetWMNormalHints (dpy, frame, &sh);
  XStoreName (dpy, frame, "Tomega");

  XClassHint ch;
  ch.res_name = (char *)"urxvt";
  ch.res_class = (char *)RESCLASS;
  XSetClassHint (dpy, frame, &ch);

  Atom prot = XInternAtom (dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols (dpy, frame, &prot, 1);

  frame_ev.set<rxvt_tab_manager, &rxvt_tab_manager::frame_cb> (this);
  frame_ev.start (display, frame);

  shutdown_ev.set<rxvt_tab_manager, &rxvt_tab_manager::shutdown_cb> (this);
  poll_ev.set<rxvt_tab_manager, &rxvt_tab_manager::poll_cb> (this);
  poll_ev.repeat = 0.5;
  poll_ev.start ();

  spawn ();

  XMapWindow (dpy, frame);
  display->flush ();
}

void
rxvt_tab_manager::spawn ()
{
  char id[32];
  snprintf (id, sizeof (id), "0x%lx", frame);

  // the embed option must precede any -e command, whose arguments
  // extend to the end of the command line
  const char *targv[256];
  int n = 0;

  if (spawn_argc < 1 || spawn_argc > 250)
    return;

  targv[n++] = spawn_argv[0];
  targv[n++] = "-embed";
  targv[n++] = id;

  for (int i = 1; i < spawn_argc; i++)
    targv[n++] = spawn_argv[i];

  targv[n] = 0; // execvp needs a NULL-terminated argv

  rxvt_term *t = new rxvt_term;
  t->init (n, targv, envv);

  tab_watcher *w = new tab_watcher;
  w->term = t;
  w->window = t->parent;
  w->closing = false;
  w->fgcmd[0] = 0;
  w->ev.set<rxvt_tab_manager, &rxvt_tab_manager::tab_cb> (this);
  w->ev.start (display, t->parent);

  // watch for title changes on the tab's toplevel
  XWindowAttributes attr;
  if (XGetWindowAttributes (display->dpy, t->parent, &attr))
    XSelectInput (display->dpy, t->parent, attr.your_event_mask | PropertyChangeMask);

  tabs.push_back (w);
  activate (tabs.size () - 1);
}

void
rxvt_tab_manager::close_tab ()
{
  if (active >= 0 && active < tabs.size ())
    {
      tabs[active]->closing = true;
      tabs[active]->term->destroy ();
    }
}

void
rxvt_tab_manager::shutdown_cb ()
{
  shutdown_ev.stop ();
  shutdown ();
}

// fill buf with the command currently in the foreground of the tab's
// tty; empty when the shell itself is foreground (idle at a prompt)
static void
tab_fgcmd (rxvt_term *term, char *buf, int buflen)
{
  buf[0] = 0;

  if (!term->cmd_pid)
    return;

  char path[64];
  snprintf (path, sizeof (path), "/proc/%d/stat", term->cmd_pid);

  FILE *f = fopen (path, "r");
  if (!f)
    return;

  char line[512];
  bool have = fgets (line, sizeof (line), f) != 0;
  fclose (f);

  if (!have)
    return;

  // ... ) state ppid pgrp session tty_nr tpgid ...
  char *p = strrchr (line, ')');
  if (!p)
    return;

  long pgrp = 0, tpgid = 0;
  sscanf (p + 2, "%*s %*s %ld %*s %*s %ld", &pgrp, &tpgid);

  if (tpgid <= 0 || tpgid == pgrp)
    return; // the shell is the foreground process

  snprintf (path, sizeof (path), "/proc/%ld/cmdline", tpgid);

  f = fopen (path, "r");
  if (!f)
    return;

  char args[256];
  int n = fread (args, 1, sizeof (args) - 1, f);
  fclose (f);

  if (n <= 0)
    return;

  args[n] = 0;

  // arguments are NUL-separated; join them with spaces
  int o = 0;
  for (int i = 0; i < n && o < buflen - 2; i++)
    {
      if (args[i])
        buf[o++] = args[i];
      else if (i + 1 < n && args[i + 1])
        buf[o++] = ' ';
    }

  buf[o] = 0;
}

// strip a redundant "user@host" prefix (our own user at our own host)
// from a tab title: "thomas@linuxboi: ~" becomes ": ~"
static void
strip_userhost (char *title)
{
  char *at = strchr (title, '@');
  if (!at)
    return;

  char *colon = strchr (at, ':');
  if (!colon)
    return;

  struct passwd *pw = getpwuid (geteuid ());
  if (!pw)
    return;

  size_t userlen = strlen (pw->pw_name);
  if ((size_t)(at - title) != userlen || strncmp (title, pw->pw_name, userlen))
    return;

  char host[256];
  if (gethostname (host, sizeof (host)))
    return;

  if (char *dot = strchr (host, '.'))
    *dot = 0; // compare against the short hostname

  size_t hostlen = colon - at - 1;
  if (hostlen != strlen (host) || strncmp (at + 1, host, hostlen))
    return;

  memmove (title, colon, strlen (colon) + 1);
}

void
rxvt_tab_manager::poll_cb ()
{
  bool changed = false;

  for (int i = tabs.size (); i--; )
    {
      if (tabs[i]->closing)
        continue;

      char cmd[sizeof (tabs[i]->fgcmd)];
      tab_fgcmd (tabs[i]->term, cmd, sizeof (cmd));

      if (strcmp (cmd, tabs[i]->fgcmd))
        {
          strcpy (tabs[i]->fgcmd, cmd);
          changed = true;
        }
    }

  if (changed)
    draw_bar ();
}

void
rxvt_tab_manager::switch_tab (int dir)
{
  if (!tabs.size ())
    return;

  int idx = next_alive ((active + dir + tabs.size ()) % tabs.size ());

  if (idx >= 0 && idx != active)
    activate (idx);
}

void
rxvt_tab_manager::activate (int idx)
{
  if (idx == active)
    return;

  if (active >= 0 && active < tabs.size () && !tabs[active]->closing)
    XUnmapWindow (display->dpy, tabs[active]->window);

  active = idx;

  layout ();
  XMapWindow (display->dpy, tabs[idx]->window);

  // make sure the map request reached the server before focusing
  XSync (display->dpy, False);
  focus_active ();

  draw_bar ();
  display->flush ();
}

void
rxvt_tab_manager::focus_active ()
{
  if (active < 0 || active >= tabs.size () || tabs[active]->closing)
    return;

  Window win = tabs[active]->window;

  // focusing a non-viewable window raises BadMatch; the frame's
  // FocusIn handler catches up once it becomes visible
  XWindowAttributes attr;
  if (!XGetWindowAttributes (display->dpy, win, &attr)
      || attr.map_state != IsViewable)
    return;

  XSetInputFocus (display->dpy, win, RevertToParent, CurrentTime);

  XEvent xev;
  xev.xclient.type = ClientMessage;
  xev.xclient.window = win;
  xev.xclient.message_type = display->xa[XA_XEMBED];
  xev.xclient.format = 32;
  xev.xclient.data.l[0] = CurrentTime;
  xev.xclient.data.l[1] = XEMBED_FOCUS_IN;
  xev.xclient.data.l[2] = XEMBED_FOCUS_CURRENT;
  xev.xclient.data.l[3] = 0;
  xev.xclient.data.l[4] = 0;
  XSendEvent (display->dpy, win, False, NoEventMask, &xev);
}

void
rxvt_tab_manager::shutdown ()
{
  tab_manager = 0;
  delete this;
}

void
rxvt_tab_manager::frame_cb (XEvent &ev)
{
  switch (ev.type)
    {
      case Expose:
        draw_bar ();
        break;

      case ConfigureNotify:
        frame_w = ev.xconfigure.width;
        frame_h = ev.xconfigure.height;
        layout ();
        break;

      case FocusIn:
        focus_active ();
        break;

      case ButtonPress:
        if (ev.xbutton.y < tab_h)
          {
            if (ev.xbutton.button == Button1)
              {
                int i = tab_at (ev.xbutton.x);
                if (i >= 0)
                  activate (i);
              }
            else if (ev.xbutton.button == Button4)
              switch_tab (-1);
            else if (ev.xbutton.button == Button5)
              switch_tab (1);
          }
        break;

      case ClientMessage:
        if (ev.xclient.data.l[0] == (long)XInternAtom (display->dpy, "WM_DELETE_WINDOW", True))
          {
            // close everything; shutdown happens when the last tab is gone
            for (int i = tabs.size (); i--; )
              {
                tabs[i]->closing = true;
                tabs[i]->term->destroy ();
              }
          }
        break;

      case DestroyNotify:
        break;
    }
}

void
rxvt_tab_manager::tab_cb (XEvent &ev)
{
  if (ev.type == DestroyNotify)
    {
      for (int i = tabs.size (); i--; )
        if (tabs[i]->window == ev.xdestroywindow.window)
          {
            tabs[i]->ev.stop (display);
            delete tabs[i];
            tabs.erase (tabs.begin () + i);
            break;
          }

      if (!tabs.size ())
        {
          // defer the self-destruct: deleting synchronously here would
          // free the display while its event loop is still dispatching
          if (!shutdown_ev.is_active ())
            shutdown_ev.start ();
          return;
        }

      if (active >= (int)tabs.size () || tabs[active]->closing)
        {
          int next = next_alive (active);

          if (next >= 0)
            activate (next);
          else
            active = -1; // everything is closing, just wait
        }

      draw_bar ();
      display->flush ();
    }
  else if (ev.type == PropertyNotify
           && (ev.xproperty.atom == XA_WM_NAME
               || ev.xproperty.atom == display->xa[XA_NET_WM_NAME]))
    draw_bar ();
}

int
rxvt_tab_manager::next_alive (int from)
{
  int n = tabs.size ();
  if (!n)
    return -1;

  for (int d = 0; d < n; d++)
    {
      int i = (from + d) % n;
      if (i < 0)
        i += n;

      if (!tabs[i]->closing)
        return i;
    }

  return -1;
}

void
rxvt_tab_manager::layout ()
{
  if (active >= 0 && active < tabs.size ())
    XMoveResizeWindow (display->dpy, tabs[active]->window,
                       0, tab_h, frame_w, frame_h - tab_h);
}

int
rxvt_tab_manager::tab_at (int x)
{
  if (!tabs.size ())
    return -1;

  int seg = frame_w / tabs.size ();
  if (seg < 1)
    seg = 1;

  int i = x / seg;

  return i > (int)tabs.size () - 1 ? (int)tabs.size () - 1 : i;
}

void
rxvt_tab_manager::draw_bar ()
{
  if (!gc)
    return;

  Display *dpy = display->dpy;
  unsigned long black = BlackPixel (dpy, DefaultScreen (dpy));
  unsigned long white = WhitePixel (dpy, DefaultScreen (dpy));

  XSetForeground (dpy, gc, black);
  XFillRectangle (dpy, frame, gc, 0, 0, frame_w, tab_h);

  int n = tabs.size ();
  if (!n)
    return;

  int seg = frame_w / n;
  if (seg < 1)
    seg = 1;

  for (int i = 0; i < n; i++)
    {
      int x0 = i * seg;

      if (i == active)
        {
          XSetForeground (dpy, gc, white);
          XFillRectangle (dpy, frame, gc, x0, 0, seg - 1, tab_h);
          XSetBackground (dpy, gc, white);
          XSetForeground (dpy, gc, black);
        }
      else
        {
          XSetForeground (dpy, gc, white);
          XSetBackground (dpy, gc, black);
          XDrawLine (dpy, frame, gc, x0 + seg - 1, 0, x0 + seg - 1, tab_h);
        }

      char *name = 0;
      bool have_name = !tabs[i]->closing && XFetchName (dpy, tabs[i]->window, &name);

      char base[128];
      snprintf (base, sizeof (base), "%s", have_name && name ? name : "Tomega");
      strip_userhost (base);

      char label[160];
      snprintf (label, sizeof (label), "%s%s%s",
                base,
                tabs[i]->fgcmd[0] ? ": " : "",
                tabs[i]->fgcmd);

      if (have_name)
        XFree (name);

      const char *title = label;
      size_t len = strlen (title);
      int maxch = (seg - 8) / font->max_bounds.width;

      if (maxch > 4)
        XDrawImageString (dpy, frame, gc, x0 + 4,
                          font->max_bounds.ascent + 3, title,
                          len > (size_t)maxch ? (size_t)maxch : len);

      if (have_name)
        XFree (name);
    }

  display->flush ();
}

#endif
