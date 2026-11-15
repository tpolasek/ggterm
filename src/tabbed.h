/*
 * tabbed.h - native tab support for rxvt-unicode
 *
 * A tab manager owns a toplevel frame window with a tab bar. Each tab
 * is a full rxvt_term embedded into the frame via the XEmbed support
 * used by -embed (rs[Rs_embed]).
 */

#ifndef RXVT_TABBED_H_
#define RXVT_TABBED_H_

#include "rxvt.h"

#if ENABLE_XEMBED

class rxvt_term;

struct tab_watcher
{
  xevent_watcher ev;
  rxvt_term *term;
  Window window;      // the tab's toplevel (term->parent), valid after the term is gone
  bool closing;       // the term is being destroyed, do not touch its window
  char fgcmd[64];     // cached foreground command of the tab, "" if the shell is idle
};

struct rxvt_tab_manager
{
  rxvt_display *display;
  Window frame;
  xevent_watcher frame_ev;
  ev::idle shutdown_ev; // deferred self-destruct, see shutdown_cb
  ev::timer poll_ev;    // polls tabs for foreground-command changes

  vector<tab_watcher *> tabs;
  int active;

  GC gc;
  XFontStruct *font;
  int tab_h;
  int frame_w, frame_h;

  // startup arguments, reused when spawning further tabs
  char **spawn_argv;
  int spawn_argc;
  const char *const *envv;

  rxvt_tab_manager ();
  ~rxvt_tab_manager ();

  void run (int argc, const char *const *argv, const char *const *envv);
  void spawn ();
  void close_tab ();
  void switch_tab (int dir);
  void activate (int idx);
  void focus_active ();
  void shutdown ();

  void frame_cb (XEvent &ev);
  void tab_cb (XEvent &ev);
  void poll_cb ();
  void shutdown_cb ();
  void draw_bar ();
  void layout ();
  int tab_at (int x);
  int next_alive (int from); // nearest tab not being destroyed
};

extern rxvt_tab_manager *tab_manager;

// returns true when tabs mode was requested on the command line
bool rxvt_tabbed_requested (int argc, const char *const *argv);

#endif
#endif
