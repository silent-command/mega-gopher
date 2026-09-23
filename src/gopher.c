/* MEGA65 Gopher client -- phase 5: navigation (selection, follow-link,
 * back, home). Builds on phase 4's parsing/rendering; see
 * REQUIREMENTS.md section 9. */
#include <stdio.h>
#include <string.h>
#include "mega65/memory.h"
#include "mega65/conio.h"
#include "gopher_screen.h"
#include "gopher_net.h"
#include "gopher_items.h"
#include "gopher_boot.h"
#include "gopher_exit.h"
#include "gopher_text.h"
#include "gopher_loc.h"
#include "gopher_config.h"
#include "gopher_f011.h"
#include "gopher_cbmdos.h"
#include "gopher_f011.h"
#include "gopher_scratch.h"

/* Definitions for the four buffers declared in gopher_scratch.h. See the
 * comment there for why these are .bss rather than fixed low-RAM. */
char scr_save_result[64];
char scr_save_suggest[17];
char scr_dos_name[17];
char scr_dos_line[80];

/* Overridable at build time so a test build can point at a LAN server
 * without editing this file -- editing it risks shipping a binary aimed
 * at somebody's Mac, which nearly happened during the native port:
 *   mos-mega65-clang -DHOME_HOST='"192.168.1.232"' -DHOME_PORT=7070 ... */
#ifndef HOME_HOST
#define HOME_HOST "gopherpedia.com"
#endif
#ifndef HOME_PORT
#define HOME_PORT 70
#endif
#ifndef HOME_SELECTOR
#define HOME_SELECTOR ""
#endif

/* LOC_HOST_LEN, LOC_SELECTOR_LEN and struct location: gopher_loc.h */

#define ITEMS_TOP_ROW 2
#define ITEMS_PER_PAGE 20
#define ROW_STATUS 23
#define ROW_HELP 24

/* Frames with no incoming bytes before a fetch is abandoned. 500 frames
 * is ~10s at 50Hz -- long enough for a slow server, short enough that a
 * silent one does not look like a crash. */
#define IDLE_FRAMES_LIMIT 500       /* ~10s between bytes once flowing */
#define FIRST_BYTE_FRAMES_LIMIT 1250 /* ~25s for a slow server to start */

/* Standard CBM/MEGA65 hardware keyboard codes, read via cgetc() from
 * $D610 (documented as delivering ASCII directly -- unrelated to CC65's
 * compile-time charmap, so no R-5-style translation risk here). Using
 * numeric constants regardless, for consistency and clarity. */
/* Bump GOPHER_VERSION here; the startup screen is the only place it is
 * printed. */
#define GOPHER_VERSION "0.4.3"

/* A four-state spinner in the top-right corner while a fetch is in
 * progress: one character, redrawn every eighth frame (about six times
 * a second), so that a slow server is visibly a slow server and not a
 * dead client. Costs one 1-byte screen write per 8 frames. */
static unsigned char spin_frame;
static void spin_step(void)
{
  static const char glyphs[4] = { 0x2e, 0x6f, 0x4f, 0x6f }; /* . o O o */
  char s[2];
  if (++spin_frame & 7)
    return;
  s[0] = glyphs[(spin_frame >> 3) & 3];
  s[1] = 0;
  gopher_putsxy(79, 0, s);
}

#define KEY_RETURN 13
#define KEY_CRSR_DOWN 17
#define KEY_CRSR_UP 145
#define KEY_HOME 19
#define KEY_STOP 3
#define KEY_FG_UPPER 0x46 /* F */
#define KEY_FG_LOWER 0x66 /* f */
#define KEY_BG_UPPER 0x42 /* B */
#define KEY_BG_LOWER 0x62 /* b */


/* This array dominates the client's static memory: each entry is a full
 * location (32 + 81 + 2 bytes), so every level costs ~117 bytes.
 *
 * 20 -> 10 when LOC_SELECTOR_LEN grew to 80 for search support.
 * 10 -> 8 when menu snapshots were added: caching menus so Back does not
 * re-fetch cost ~1KB of code, and the depth is where that is cheapest to
 * pay for. Eight levels of instant, position-preserving Back is a better
 * deal than ten that each cost a network round-trip.
 *
 * The depth is set from measurement, not guesswork: with 8 levels the gap
 * between __bss_end and the stack top is 584 bytes, and the deepest stack
 * use observed on hardware -- across nested menus, articles, search, the
 * pager and the error paths -- was 133 bytes. See TEST-RESULTS.md.
 *
 * Must not exceed ITEM_SNAPSHOT_SLOTS, or deep levels lose their cache
 * and silently fall back to re-fetching. */
#define MAX_HISTORY 8
/* The history stack lives in attic RAM, not here.
 *
 * Eight entries is ~920 bytes, which the $2001-$d000 window cannot spare
 * once the save-to-disk code is in. It is also the coldest data in the
 * program -- touched twice per navigation, never in a draw loop -- so a
 * DMA copy per push/pop is free in practice.
 *
 * Placed clear of the text store ($8000000-$804e200) and the menu
 * snapshots ($8060000 + 8 x 32000). Gated on the same probe as the
 * snapshots: without attic RAM there is no Back, which is a real but
 * acceptable degradation on hardware that has never actually turned up. */
#define HISTORY_ATTIC_BASE 0x80a0000UL

static void history_store(unsigned char slot, const struct location *loc)
{
  lcopy((unsigned long)(unsigned int)loc,
      HISTORY_ATTIC_BASE + (unsigned long)slot * sizeof(struct location),
      sizeof(struct location));
}

static void history_fetch(unsigned char slot, struct location *loc)
{
  lcopy(HISTORY_ATTIC_BASE + (unsigned long)slot * sizeof(struct location),
      (unsigned long)(unsigned int)loc, sizeof(struct location));
}

/* One buffer for "format a line, draw it, done".
 *
 * fetch_menu, say_net_error, draw_menu_page, view_text's status line and
 * save_article each had their own 80-100 byte static; together that was
 * ~430 bytes for five things that are never live at the same time. The
 * one exception is read_line(), which keeps its own buffer because a
 * caller builds a prompt in here and read_line echoes into its own while
 * that prompt is still in use. */
#define uiline SCR_UILINE




/* Where the cursor was in each history level. Restored on Back so coming
 * out of an article or a submenu puts you back where you were rather than
 * at the top -- on a 64-item menu that was a lot of scrolling. */
static unsigned char hist_selected[MAX_HISTORY];
static unsigned char hist_page[MAX_HISTORY];
static unsigned char history_count = 0;

/* In fixed low RAM, not .bss; see gopher_scratch.h. */
#define home_loc (*SCR_HOME_LOC)
#define current_loc (*SCR_CUR_LOC)

static unsigned char last_frame = 0;

static unsigned char tick(void)
{
  if (PEEK(0xd7fa) != last_frame) {
    last_frame = PEEK(0xd7fa);
    return 1;
  }
  return 0;
}

static void newline(void)
{
  gotoxy(0, wherey() + 1);
}

static void sayln(const char *s)
{
  gopher_puts(s);
  newline();
}

/* Screen-code translation now lives in gopher_screen.c (see
 * gopher_puts/gopher_putsxy there): text is kept as plain ASCII
 * throughout this file and converted once, at draw time. */

/* NOTE: sprintf() with %s crashes under llvm-mos on this target.
 *
 * Isolated on hardware by stepping border-colour markers through
 * fetch_menu: clrscr(), sayln() and newline() all completed, and the very
 * next statement -- sprintf(out, "Connecting to %s...", host) -- died,
 * scribbling the VIC-IV registers on the way down (which is why the
 * symptom looked like a display fault rather than a string bug).
 * Conversions that take no pointer argument, such as %u, are fine and are
 * still used below.
 *
 * These strings are simple concatenations, so they are built with
 * strcpy/strcat instead. That also avoids pulling the variadic printf
 * machinery into an 8-bit binary for no benefit. */
static void set_location(struct location *loc, const char *host, const char *selector, unsigned int port)
{
  strncpy(loc->host, host, LOC_HOST_LEN);
  loc->host[LOC_HOST_LEN] = 0;
  strncpy(loc->selector, selector, LOC_SELECTOR_LEN);
  loc->selector[LOC_SELECTOR_LEN] = 0;
  loc->port = port;
}

/* Parses one raw RFC 1436 menu line (type + display \t selector \t host
 * \t port) and stores it via item_add(). Display text is translated for
 * screen display; selector/host are left as raw ASCII since they're
 * protocol data we'll send back to a server, not shown as-is. */
static void parse_and_store_line(char *line, unsigned char len)
{
  char *folded = SCR_FOLDED;
  unsigned char type;
  char *display, *selector, *host, *portstr, *p;
  unsigned int port;

  if (len == 0)
    return;
  if (len == 1 && line[0] == 0x2e) /* '.' */
    return;

  type = line[0];
  display = line + 1;
  selector = "";
  host = "";
  portstr = "";

  for (p = display; *p && *p != 0x09 /* '\t' */; p++)
    ;
  if (*p == 0x09) {
    *p = 0;
    selector = p + 1;
    for (p = selector; *p && *p != 0x09; p++)
      ;
    if (*p == 0x09) {
      *p = 0;
      host = p + 1;
      for (p = host; *p && *p != 0x09; p++)
        ;
      if (*p == 0x09) {
        *p = 0;
        portstr = p + 1;
      }
    }
  }

  port = 0;
  for (p = portstr; *p >= 0x30 && *p <= 0x39; p++) /* '0'-'9' */
    port = port * 10 + (*p - 0x30);
  if (port == 0)
    port = 70;

  /* Display text is kept as plain ASCII here. Translation to screen
   * codes happens once, at draw time, via gopher_putsxy(). Translating
   * early was what created the "literals are magic, network data is not"
   * split behind R-5, R-6 and the title-bar bug.
   *
   * UTF-8 is folded here rather than at draw time so the stored string is
   * already one byte per character -- otherwise the ITEM_DISPLAY_LEN
   * truncation below would cut multi-byte sequences in half. */
  gopher_fold_utf8(folded, display, ITEM_DISPLAY_LEN);
  display = folded;
  for (p = display; *p; p++) {
    if (*p == 0x7b) /* '{' */
      *p = 0x28;    /* '(' */
  }
  if (strlen(display) > ITEM_DISPLAY_LEN)
    display[ITEM_DISPLAY_LEN] = 0;

  item_add(type, display, selector, host, port);
}

/* --- typed input and URL parsing (FR-1) ------------------------------ */

#define KEY_DELETE 20
#define KEY_GO_UPPER 0x47 /* G: get the selected item to disk */
#define KEY_GO_LOWER 0x67 /* g */
#define KEY_ADDR_UPPER 0x41 /* A: go to an address */
#define KEY_ADDR_LOWER 0x61 /* a */
#define KEY_HELP_CODE 0x1f /* HELP: the start screen, from anywhere */
#define KEY_MARK_UPPER 0x4d /* M */
#define KEY_MARK_LOWER 0x6d /* m */
#define KEY_P_UPPER 0x50 /* P */
#define KEY_P_LOWER 0x70 /* p */
#define KEY_S_UPPER 0x53 /* S */
#define KEY_S_LOWER 0x73 /* s */

/* Reads a line of typed input at (0,row). Returns 1 if the user pressed
 * Return, 0 if they pressed Stop to cancel. Echoes as it goes, since the
 * client has no cursor of its own. */
static unsigned char read_line(unsigned char row, const char *prompt, char *out, unsigned char maxlen)
{
  char *shown = SCR_SHOWN;
  unsigned char len;
  unsigned char key;

  /* `out` is used as the initial value, not cleared: it lets a caller
   * offer an editable default (the save dialog pre-fills a filename
   * derived from the selector). Callers wanting an empty field clear it
   * themselves. */
  out[maxlen] = 0;
  len = (unsigned char)strlen(out);

  /* Flush once on entry, to drop whatever keystroke opened this prompt --
   * NOT on every iteration. Flushing inside the loop discarded anything
   * typed while the previous redraw was in progress, so fast typing (and
   * key repeat) silently lost characters. */
  flushkeybuf();

  for (;;) {
    strcpy(shown, prompt);
    strcat(shown, out);
    strcat(shown, "_"); /* stand-in cursor */
    /* clear the rest of the line so deleting leaves nothing behind */
    while (strlen(shown) < 79)
      strcat(shown, " ");
    shown[79] = 0;
    gopher_putsxy(0, row, shown);

    key = cgetc();

    if (key == KEY_RETURN)
      return len > 0;
    if (key == KEY_STOP)
      return 0;
    if (key == KEY_DELETE) {
      if (len)
        out[--len] = 0;
      continue;
    }
    /* accept printable ASCII only */
    if (key >= 0x20 && key < 0x7f && len < maxlen) {
      out[len++] = (char)key;
      out[len] = 0;
    }
  }
}

/* Parses "gopher://host:port/selector", "host:port/selector", "host/sel"
 * or bare "host" into a location. Returns 1 on success.
 *
 * Deliberately lenient: this is a convenience shortcut, and a user typing
 * a URL on a C65 keyboard should not have to get the scheme prefix exact. */
/* Recognises the gopher type character that a URL carries between the
 * host and the selector: gopher://host:port/<type><selector>.
 *
 * Accepted only when it is a plausible type AND the next character is '/'
 * or the end of the string. Without that guard "host//selector" -- the
 * form the client's own title bar shows, and what users copy back in --
 * would read '/' as the type and mangle the selector. With it:
 *
 *   host/1/foo   type '1', selector "/foo"
 *   host/0/foo   type '0', selector "/foo"   (a text file)
 *   host//foo    no type,  selector "/foo"
 *   host/foo     no type,  selector "foo"
 */
static unsigned char is_gopher_type(unsigned char c)
{
  return (unsigned char)((c >= 0x30 && c <= 0x39) || c == 0x2b ||
                         (c >= 0x61 && c <= 0x7a) || (c >= 0x41 && c <= 0x5a));
}

static unsigned char parse_url(const char *url, struct location *loc,
                               unsigned char *type_out)
{
  /* Parsed straight into *loc rather than via two statics -- the copy
   * they existed for was the last thing set_location() did anyway. A
   * failed parse can leave *loc half-written, which is harmless: every
   * caller either passes a throwaway local or re-prompts. */
  unsigned int port = 70;
  unsigned char n = 0;
  const char *p = url;

  while (*p == 0x20) /* skip leading spaces */
    p++;

  /* optional scheme */
  if (!strncmp(p, "gopher://", 9))
    p += 9;

  /* host, up to ':' or '/' */
  while (*p && *p != 0x3a && *p != 0x2f && n < LOC_HOST_LEN)
    loc->host[n++] = *p++;
  loc->host[n] = 0;
  if (n == 0)
    return 0;

  /* optional :port */
  if (*p == 0x3a) {
    p++;
    port = 0;
    while (*p >= 0x30 && *p <= 0x39) {
      port = port * 10 + (unsigned int)(*p - 0x30);
      p++;
    }
    if (port == 0)
      port = 70;
  }

  /* remainder is the selector; a bare host means the root menu */
  n = 0;
  if (type_out)
    *type_out = GOPHER_TYPE_MENU;
  if (*p == 0x2f) {
    p++;
    if (*p && is_gopher_type((unsigned char)*p) && (p[1] == 0x2f || p[1] == 0)) {
      if (type_out)
        *type_out = (unsigned char)*p;
      p++;
    }
  }
  while (*p && n < LOC_SELECTOR_LEN)
    loc->selector[n++] = *p++;
  loc->selector[n] = 0;

  loc->port = port;
  return 1;
}

/* Startup site chooser.
 *
 * Replaces going straight to a compiled-in default: the user picks where
 * to start, including typing an address. Option 1 reuses the same
 * read_line/parse_url path as the in-app G key, so there is one URL
 * parser and one line editor rather than two.
 *
 * Loops until a valid choice is made -- there is nowhere sensible to fall
 * back to, since this runs before any page has been fetched. */

/* Bookmarks read from GOPHER.CFG on the boot disk at startup. The chooser
 * offers them as options 4 upwards; M on a menu toggles the current page
 * in and out of the list. */
static struct location bookmarks[CFG_MAX_BOOKMARKS];
static unsigned char bookmark_count = 0;

/* Set when the menu loop wants the start screen back; see the Home key. */
static unsigned char want_chooser = 0;

/* Same host, port and selector: what "already bookmarked" means. */
static unsigned char bookmark_index(const struct location *loc)
{
  unsigned char i;

  for (i = 0; i < bookmark_count; i++)
    if (bookmarks[i].port == loc->port &&
        !strcmp(bookmarks[i].host, loc->host) &&
        !strcmp(bookmarks[i].selector, loc->selector))
      return i;
  return CFG_MAX_BOOKMARKS; /* not found */
}

/* Defined below; the start chooser needs it so a text URL typed there
 * is shown as text rather than menu-parsed into nonsense. */
static void fetch_text_to(const char *host, const char *selector,
                          unsigned int port, unsigned char view);
#define fetch_text(h, s, p) fetch_text_to(h, s, p, 1)

/* Bounded uiline builders, defined below with say_net_error. */
static void ui_set(const char *s);
static void ui_add(const char *s);
static void ui_add_uint(unsigned int v);

/* The start screen: fixed options, then the saved bookmarks.
 *
 * Selection is typed and confirmed with Return rather than being a single
 * keypress. A single key cannot address more than nine entries, and the
 * bookmark list is no longer capped below that. Typing also gives the
 * delete syntax somewhere to live: D before a number removes that
 * bookmark, so entries can be managed from the screen that lists them
 * instead of only by re-visiting a page and toggling M. */
static void choose_start_site(struct location *loc)
{
  char *urlbuf = SCR_URLBUF2;
  char *inbuf = SCR_SLINE;
  unsigned char row, b;

  for (;;) {
    clrscr();
    gotoxy(0, 0);
    sayln("MEGA65 Gopher Client");
    newline();
    sayln("Where would you like to start?");
    newline();
    sayln("  1. Enter a Gopher site");

    /* gopherpedia and floodgap used to be hard-coded options 2 and 3.
     * They are ordinary bookmarks now, seeded on a disk that has never
     * been configured, so they can be deleted like any other. */
    for (b = 0; b < bookmark_count; b++) {
      ui_set("  ");
      ui_add_uint((unsigned int)(b + 2));
      ui_add(". ");
      ui_add(bookmarks[b].host);
      if (bookmarks[b].selector[0]) {
        ui_add(" ");
        ui_add(bookmarks[b].selector);
      }
      sayln(uiline);
    }

    newline();
    sayln("Select the number of a bookmark and press Return.");
    sayln("Press Q or RUN/STOP to leave for BASIC.");
    if (bookmark_count)
      sayln("Type D before the number to delete it, e.g. D2.");

    /* Placed from wherey() rather than a constant: the list grows and
     * shrinks, and a fixed row would either overlap it or leave a gap. */
    row = (unsigned char)(wherey() + 1);

    inbuf[0] = 0;
    if (!read_line(row, "> ", inbuf, 6)) {
      /* RUN/STOP at the top level leaves, as Q does. The exit is a
       * software reset and the ROM enters its monitor if it finds
       * RUN/STOP held; half a second for the finger first. */
      unsigned char n = 0, last = *(volatile unsigned char *)0xd7fa;
      while (n < 30)
        if (*(volatile unsigned char *)0xd7fa != last) { last = *(volatile unsigned char *)0xd7fa; n++; }
      gopher_exit_to_basic();
      continue; /* not reached */
    }
    /* Q leaves for BASIC. Not RUN/STOP: the exit is a software reset, and
     * the ROM enters its monitor when it finds RUN/STOP held at reset,
     * which a finger still on the key a few milliseconds later is. */
    if ((inbuf[0] == 0x51 || inbuf[0] == 0x71) && inbuf[1] == 0) {
      gopher_exit_to_basic();
      continue; /* not reached */
    }

    {
      char *p = inbuf;
      unsigned int n = 0;
      unsigned char digits = 0, del = 0;

      while (*p == 0x20)
        p++;
      if (*p == 0x44 || *p == 0x64) { /* 'D' or 'd' */
        del = 1;
        p++;
        while (*p == 0x20)
          p++;
      }
      while (*p >= 0x30 && *p <= 0x39) {
        n = n * 10 + (unsigned int)(*p - 0x30);
        p++;
        digits++;
      }
      if (!digits)
        continue; /* empty or unparseable: just redraw */

      if (del) {
        if (n >= 2 && n < (unsigned int)(2 + bookmark_count)) {
          unsigned char at = (unsigned char)(n - 2), j;
          for (j = at; j + 1 < bookmark_count; j++)
            bookmarks[j] = bookmarks[j + 1];
          bookmark_count--;
          /* The write tears down the 80-column mode; the loop redraws
           * from the top, but the mode has to come back first. */
          {
            unsigned char ok = config_save(bookmarks, bookmark_count,
                                           boot_drive);
            gopher_screen_init();
            if (!ok) {
              /* Reported rather than swallowed: a delete that vanishes
               * from the screen but not from the disk comes back on the
               * next run, which is worse than an error. */
              ui_set("Could not write bookmarks to drive ");
              /* device 8 is F011 drive 0; SAVE_DEVICE_MIN is defined
               * further down the file than this. */
              ui_add_uint((unsigned int)(boot_drive + 8));
              gopher_putsxy(0, 0, uiline);
              flushkeybuf();
              cgetc();
            }
          }
        }
        continue;
      }

      if (n >= 2 && n < (unsigned int)(2 + bookmark_count)) {
        *loc = bookmarks[n - 2];
        return;
      }
      if (n != 1)
        continue; /* out of range: redraw rather than guess */
    }

    /* 1: type a site */
    clrscr();
    gotoxy(0, 0);
    sayln("MEGA65 Gopher Client");
    newline();
    sayln("Examples:  gopherpedia.com");
    sayln("           gopher://host:port/selector");
    newline();
    urlbuf[0] = 0;
    {
      unsigned char utype = GOPHER_TYPE_MENU;
      if (read_line(8, "Go to: ", urlbuf, SCR_URLBUF_LEN - 1) &&
          parse_url(urlbuf, loc, &utype)) {
        if (utype != GOPHER_TYPE_TEXT)
          return;
        /* A text URL is not a start site, but silently fetching it as a
         * menu would garble it -- menu parsing eats the first character
         * of every line as an item type. Show it, then ask again. */
        fetch_text(loc->host, loc->selector, loc->port);
      }
    }
    /* cancelled or unparseable -- show the menu again */
  }
}

static void append_uint(char *dst, unsigned int value);          /* below */
static unsigned char find_next_selectable(unsigned char from);   /* below */

/* Pops one history level into *loc, restoring the cursor position and, if
 * the snapshot survived, the menu itself. Returns 1 if the item store was
 * restored (so the caller can skip the network fetch), 0 if it must
 * re-fetch. */
static unsigned char history_pop(struct location *loc, unsigned char *selected,
                                 unsigned char *page_start)
{
  history_count--;
  history_fetch(history_count, loc);
  if (!item_snapshot_load(history_count))
    return 0;
  *selected = hist_selected[history_count];
  *page_start = hist_page[history_count];
  if (item_count == 0 || *selected >= item_count) {
    *selected = find_next_selectable(item_count - 1);
    *page_start = 0;
  }
  return 1;
}

/* Records the current page on the history stack: its location, the cursor
 * position, and a snapshot of the item store so Back can redraw it
 * without going back to the server. */
static void history_push(const struct location *loc, unsigned char selected,
                         unsigned char page_start)
{
  if (history_count >= MAX_HISTORY)
    return;
  hist_selected[history_count] = selected;
  hist_page[history_count] = page_start;
  item_snapshot_save(history_count);
  history_store(history_count, loc);
  history_count++;
}

/* Reports a failed fetch with the reason, the host, and -- when we got as
 * far as having one -- the address we actually tried. Naming the address
 * matters: it separates "this name resolves to the wrong place" from
 * "that machine is not answering", which the old single "connection
 * failed" line could not. Callers do their own "press any key" wait. */
/* Bounded builders for uiline.
 *
 * These replace a strcpy/strcat chain, for two reasons.
 *
 * 1. It hung the client. `strcat(uiline, gopher_net_strerror())` compiled
 *    to a scan loop at $497f whose pointer bytes ($74/$75) were never
 *    updated inside the loop, so it re-read one byte forever. Every
 *    network failure therefore froze the machine on the "Connecting to
 *    ..." screen instead of reporting the error -- the connect timeout had
 *    already fired correctly; it was the reporting that never returned.
 *    Found by reading the PC over the serial monitor while hung
 *    ($4986-$4998, inside say_net_error) after instrumentation had ruled
 *    out every far call into the stack. See REQUIREMENTS.md 2.34.
 *
 * 2. It could overflow. `host` can be up to SCR_URLBUF_LEN-1 (127) bytes
 *    and uiline is SCR_UILINE_LEN (100), with no bound anywhere in the
 *    old code.
 *
 * ui_len is kept rather than re-scanned so nothing here walks a string
 * twice. */
static unsigned char ui_len;

static void ui_set(const char *s)
{
  ui_len = 0;
  while (*s && ui_len < SCR_UILINE_LEN - 1)
    uiline[ui_len++] = *s++;
  uiline[ui_len] = 0;
}

static void ui_add(const char *s)
{
  while (*s && ui_len < SCR_UILINE_LEN - 1)
    uiline[ui_len++] = *s++;
  uiline[ui_len] = 0;
}

static void ui_add_uint(unsigned int v)
{
  char digits[6];
  unsigned char n = 0;

  if (v == 0)
    digits[n++] = 0x30;
  while (v) {
    digits[n++] = (char)(0x30 + (v % 10));
    v /= 10;
  }
  while (n && ui_len < SCR_UILINE_LEN - 1)
    uiline[ui_len++] = digits[--n];
  uiline[ui_len] = 0;
}

static void say_net_error(const char *host, const unsigned char *ip,
                          unsigned int port)
{
  ui_set("ERROR: ");
  ui_add(gopher_net_strerror());
  sayln(uiline);

  ui_set("  Host: ");
  ui_add(host);
  sayln(uiline);

  if (ip) {
    ui_set("  Addr: ");
    ui_add_uint(ip[0]);
    ui_add(".");
    ui_add_uint(ip[1]);
    ui_add(".");
    ui_add_uint(ip[2]);
    ui_add(".");
    ui_add_uint(ip[3]);
    ui_add(":");
    ui_add_uint(port);
    sayln(uiline);
  }

  ui_set("  Tried ");
  ui_add_uint(gopher_net_attempts);
  ui_add(gopher_net_attempts == 1 ? " time" : " times");
  sayln(uiline);
}

/* Connects to loc, sends its selector, and parses the response into the
 * item store. Returns 1 on success, 0 on any failure (with an error
 * message already shown). */
static unsigned char fetch_menu(struct location *loc)
{
  unsigned char ip[4];
  char *linebuf = SCR_MENU_LINE;
  unsigned char linelen = 0;
  unsigned char pump_result;
  unsigned char b, i;
  unsigned char got_any;
  unsigned char received_any;
  unsigned char closing = 0;
  unsigned int idle_frames;
  clrscr();
  /* clrscr() does not reliably reset the cursor here, so anything printed
   * with sayln() afterwards lands wherever the previous screen left it --
   * which put the first line of a text file at the bottom-right corner and
   * ran the rest off-screen. Home the cursor explicitly. */
  gotoxy(0, 0);
  sayln("MEGA65 Gopher Client");
  newline();
  strcpy(uiline, "Connecting to ");
  strcat(uiline, loc->host);
  strcat(uiline, "...");
  sayln(uiline);

  if (!gopher_dns_resolve(loc->host, ip)) {
    say_net_error(loc->host, 0, 0);
    return 0;
  }
  if (!gopher_tcp_connect(ip, loc->port)) {
    /* The address may have come from the DNS cache and gone stale; drop it
     * so the next attempt resolves afresh rather than failing forever. */
    gopher_dns_invalidate(loc->host);
    say_net_error(loc->host, ip, loc->port);
    return 0;
  }
  if (!gopher_tcp_send_selector(loc->selector)) {
    say_net_error(loc->host, ip, loc->port);
    return 0;
  }

  item_clear_all();

  /* Bounded receive. A server that accepts the connection and then sends
   * nothing used to hang this loop forever on a freshly cleared screen,
   * which is exactly what a rate-limiting server does (observed against
   * gopherpedia after many rapid fetches -- see R-8c). Give up after
   * IDLE_FRAMES_LIMIT frames with no new bytes; whatever arrived is
   * still parsed and shown, so a truncated menu beats a dead client. */
  idle_frames = 0;
  received_any = 0;
  while (1) {
    if (tick()) {
      spin_step();
      pump_result = gopher_tcp_pump();
      got_any = 0;
      for (i = 0; i < 200; i++) {
        if (!gopher_tcp_recv_byte(&b))
          break;
        got_any = 1;
        received_any = 1;
        if (b == 0x0d) /* '\r' */
          continue;
        if (b == 0x0a) { /* '\n' */
          /* Terminate before parsing. parse_and_store_line() walks the
           * buffer with *p tests, and its last field (the port) has no
           * following tab to stop at -- so without this it ran past the
           * end of the current line into whatever a previous, longer line
           * left behind. Any stale digits there were appended to the port:
           * a real port of 7072 followed by a leftover "72" parsed as
           * 51912, and the client then connected to that, which the host
           * refused. That is a large part of the intermittent
           * "connection failed" reports -- it depends on the relative
           * lengths of adjacent menu lines, so it looks random and is
           * site-specific. A line with fewer than three tabs could pick up
           * a corrupt host or selector the same way. */
          linebuf[linelen] = 0;
          parse_and_store_line(linebuf, linelen);
          linelen = 0;
          continue;
        }
        if (linelen < SCR_LINE_LEN - 1)
          linebuf[linelen++] = b;
      }
      idle_frames = got_any ? 0 : (idle_frames + 1);
      if (pump_result)
        closing = 1;

      /* The peer closing does NOT mean we have seen all its data: bytes
       * already sitting in the stack's receive ring still have to be
       * drained. Breaking straight uiline on pump_result threw them away --
       * a 60-line text file arrived as 31 lines. So once closure is
       * signalled, keep draining until a whole frame yields nothing. */
      /* Allow longer for the FIRST byte than between bytes. A busy server
       * can take many seconds to start replying while streaming quickly
       * once it does; one limit either cuts those off or makes every dead
       * connection slow. The /latemenu case in
       * tools/adversarial_server.py (10s of silence, then a valid menu)
       * was reported as an empty menu under the old single 10s limit. */
      if ((closing && !got_any) ||
          idle_frames > (received_any ? IDLE_FRAMES_LIMIT
                                      : FIRST_BYTE_FRAMES_LIMIT)) {
        if (linelen) {
          linebuf[linelen] = 0; /* same reason as above */
          parse_and_store_line(linebuf, linelen);
        }
        break;
      }
    }
  }

  /* A connection that produced no bytes at all is a failure, not an empty
   * menu. Drawing "(empty menu)" for it told the user the server replied
   * with nothing, when in fact it never replied. */
  if (!received_any) {
    gopher_net_error = GNERR_NO_DATA;
    say_net_error(loc->host, ip, loc->port);
    return 0;
  }
  return 1;
}

/* CBM DOS filenames are 16 characters and a much smaller alphabet than a
 * gopher selector, so build a suggestion from the selector's last path
 * component: keep letters and digits, fold anything else to '-', and
 * upper-case it the way a directory listing will show it anyway.
 * ':' ',' '=' '*' '?' are not merely ugly here -- CBM DOS treats them as
 * syntax, so a name containing one is not a name. */
#define SAVE_NAME_LEN 16
#define save_suggestion SCR_SAVE_SUGGEST

static void build_save_suggestion(const char *selector)
{
  const char *p;
  const char *last = selector;
  unsigned char n = 0;
  unsigned char ch;

  for (p = selector; *p; p++)
    if (*p == 0x2f) /* '/' */
      last = p + 1;

  for (p = last; *p && n < SAVE_NAME_LEN; p++) {
    ch = (unsigned char)*p;
    if (ch >= 0x61 && ch <= 0x7a) /* 'a'-'z' -> upper */
      ch = (unsigned char)(ch - 32);
    if ((ch >= 0x41 && ch <= 0x5a) || (ch >= 0x30 && ch <= 0x39))
      save_suggestion[n++] = (char)ch;
    else if (n > 0 && save_suggestion[n - 1] != 0x2d) /* '-' */
      save_suggestion[n++] = 0x2d;
  }
  while (n > 0 && save_suggestion[n - 1] == 0x2d) /* no trailing '-' */
    n--;
  save_suggestion[n] = 0;

  if (n == 0)
    strcpy(save_suggestion, "ARTICLE");
}

/* Fetches a type-0 text item into the banked line store, then pages
 * through it (FR-9/10/11).
 *
 * Phase 5 streamed lines straight to the screen, so a file longer than
 * one screenful scrolled past unread -- verified on hardware, where a
 * 30-line file showed only its tail. Buffering first also means the
 * connection is closed before the user starts reading, rather than being
 * held open for as long as they linger. */
#define TEXT_ROWS 23
#define TEXT_STATUS_ROW 24

/* Append an unsigned decimal to a string, without printf.
 *
 * sprintf() is not trustworthy on this target: "%s" crashes outright
 * (see the note above), and the three-argument "%u-%u of %u" status line
 * in the pager corrupted the screen and hung the client while the
 * two-argument menu status line was fine. Rather than keep guessing
 * which format strings are safe, the UI now formats numbers itself. */
static void append_uint(char *dst, unsigned int value)
{
  char digits[6];
  unsigned char n = 0;
  unsigned char len = (unsigned char)strlen(dst);

  if (value == 0)
    digits[n++] = 0x30; /* '0' */
  while (value) {
    digits[n++] = (char)(0x30 + (value % 10));
    value /= 10;
  }
  while (n)
    dst[len++] = digits[--n];
  dst[len] = 0;
}

#define KEY_SAVE_UPPER 0x53 /* S */
#define KEY_SAVE_LOWER 0x73 /* s */
#define SAVE_DEVICE_MIN 8
#define SAVE_DEVICE_MAX 9
#define KEY_8 0x38
#define KEY_9 0x39

/* Remembered between saves: someone filing several articles onto the same
 * disk should not have to re-choose every time. Held as an F011 drive
 * number (0 or 1), which is what the disk layer wants; device 8 is drive 0
 * and device 9 is drive 1. */
static unsigned char save_drive = 0;

/* Saving is DISABLED: BAM allocation is unreliable and has been observed
 * handing out blocks already in use, overwriting another file. See
 * REQUIREMENTS.md 2.24 before re-enabling.
 *
 * The UI, the filename derivation and the streaming writer are all
 * finished and correct. What does not work is calling the KERNAL at all
 * from this program, and that was measured rather than guessed:
 *
 *   interrupts ENABLED  -> hangs instantly. This program loads at $2001
 *                          and is ~41KB, occupying $a000-$c0f1; the KERNAL
 *                          IRQ handler needs ROM mapped where our code and
 *                          data live, so servicing an interrupt runs our
 *                          bytes as if they were ROM.
 *   interrupts MASKED   -> KERNAL OPEN never returns, in the full client
 *                          and in a 2.7KB build with a disk attached.
 *
 * KERNAL disk I/O needs interrupts; interrupts need a ROM layout this
 * program occupies. The two cannot coexist, so the route out is to bypass
 * the KERNAL entirely (hypervisor calls, or driving F011 directly), not to
 * keep adjusting the banking. Until then S is not offered. */
#define SAVE_ENABLED 1

/* Writes the article currently in the text store to disk.
 *
 * The screen is expected NOT to survive this. gopher_config.c records why:
 * the KERNAL sequence works standalone but $d030 carries VIC-III/IV
 * configuration as well as ROM banking, so switching it fights the
 * 80-column screen, and doing that underneath a live display destabilised
 * the machine twice. A save is discrete and user-initiated, so unlike
 * config persistence it can simply plan to lose the screen and rebuild it
 * afterwards -- which is what this does.
 *
 * The three colour registers are saved and put back before
 * gopher_screen_init() re-reads them, so a save does not silently undo
 * the user's F/B colour choices. */
/* ASCII to PETSCII for text written to disk.
 *
 * The text store holds plain ASCII, but a SEQ file on a CBM disk is
 * expected to be PETSCII, and that is what BASIC's PRINT and every other
 * CBM tool will assume. Writing ASCII made saved articles unreadable:
 * in the default uppercase/graphics mode, ASCII lowercase ($61-$7a)
 * lands on graphics characters, so half of every line came out as junk.
 *
 * The conventional mapping swaps the two letter ranges:
 *   'A'-'Z' ($41-$5a) -> $c1-$da
 *   'a'-'z' ($61-$7a) -> $41-$5a
 * Digits, punctuation and the CR line terminator pass through.
 *
 * That reads correctly in lower/uppercase mode, and stays legible (as
 * all capitals) in uppercase/graphics mode -- unlike ASCII, which is
 * unreadable in one of them. */
static unsigned char ascii_to_petscii(unsigned char c)
{
  if (c >= 0x41 && c <= 0x5a)
    return (unsigned char)(c + 0x80);
  if (c >= 0x61 && c <= 0x7a)
    return (unsigned char)(c - 0x20);
  return c;
}

/* Asks which drive to write to, at `row`. Returns 0 if the user cancelled.
 *
 * The MEGA65 exposes two: device 8 is F011 drive 0, device 9 is drive 1
 * (a second image, or the real floppy). Return keeps the last choice, so
 * writing several files to one disk is a single keypress each time.
 *
 * Whether a disk is actually present is not pre-checked. The obvious test
 * -- DISKIN in $d083 -- is unreliable here for the same reason SPINUP is:
 * against a mounted .d81 the drive is virtualised and the status bits do
 * not mean what they would on real hardware (see f011_start). A wrong
 * pre-check would refuse a write that would have worked, so the write is
 * attempted and any failure reported instead. */
/* Remembered like the drive, so a run of downloads from one site is not a
 * run of identical answers. SEQ is the default: a gopher type-9 item is
 * generic binary, and LOAD reads a PRG's first two bytes as its load
 * address, so mislabelling data as PRG makes LOAD scatter it into memory.
 * PRG is right only when the file really is a program. */
static unsigned char save_as_prg = 0;

static unsigned char choose_filetype(unsigned char row)
{
  ui_set("File type: P)RG or S)EQ? (Return = ");
  ui_add(save_as_prg ? "PRG" : "SEQ");
  ui_add(", Stop cancels)");
  gopher_putsxy(0, row, uiline);

  flushkeybuf();
  for (;;) {
    unsigned char k = cgetc();
    if (k == KEY_STOP)
      return 0;
    if (k == KEY_RETURN)
      return 1;
    if (k == KEY_P_UPPER || k == KEY_P_LOWER) {
      save_as_prg = 1;
      return 1;
    }
    if (k == KEY_S_UPPER || k == KEY_S_LOWER) {
      save_as_prg = 0;
      return 1;
    }
  }
}

/* Draws the drive prompt. Separate because the chooser redraws it after
 * rejecting a real floppy. */
static void draw_drive_prompt(unsigned char row)
{
  ui_set("Write to drive 8 or 9? (Return = ");
  ui_add_uint((unsigned int)(save_drive + SAVE_DEVICE_MIN));
  ui_add(", Stop cancels)");
  /* Pad to the width of the line: this replaces the longer "real floppy"
   * refusal, whose tail would otherwise show past the end of the prompt. */
  while (ui_len < 78)
    uiline[ui_len++] = 0x20;
  uiline[ui_len] = 0;
  gopher_putsxy(0, row, uiline);
}

static unsigned char choose_drive(unsigned char row)
{
  draw_drive_prompt(row);

  flushkeybuf();
  for (;;) {
    unsigned char k = cgetc();
    unsigned char want;

    if (k == KEY_STOP)
      return 0;
    if (k == KEY_RETURN)
      want = save_drive;
    else if (k == KEY_8)
      want = 0;
    else if (k == KEY_9)
      want = 1;
    else
      continue; /* anything else: keep waiting rather than guessing */

    /* Writing to a real floppy is refused rather than left to fail
     * confusingly. Two genuine driver bugs were found and fixed while
     * chasing it -- the sector buffer pointers were never reset (NOBUF),
     * and a real 1581 keeps the directory track's header and BAM on the
     * opposite physical side from a .d81 -- but reads from real media
     * stayed intermittent even with six retries, so the path is not
     * trustworthy. Mounted images work on either drive.
     * See REQUIREMENTS.md 2.47. */
    if (!f011_drive_is_image(want)) {
      ui_set("Drive ");
      ui_add_uint((unsigned int)(want + SAVE_DEVICE_MIN));
      ui_add(" is a real floppy - only mounted .d81 images are supported.");
      gopher_putsxy(0, row, uiline);
      flushkeybuf();
      cgetc();
      draw_drive_prompt(row);
      continue;
    }

    save_drive = want;
    return 1;
  }
}

/* Outcome of the last save, shown in the pager status line until the
 * user's next keystroke. Empty when there is nothing to report. */
#define save_result SCR_SAVE_RESULT
static unsigned char save_err;


#if SAVE_ENABLED
static void save_article(void)
{
  static char name[SAVE_NAME_LEN + 1];
  unsigned char ok;

  if (text_line_count == 0) {
    strcpy(save_result, "Nothing to save.");
    return;
  }

  /* Pre-filled and editable: Return accepts the derived name, and it can
   * be edited or cleared. read_line() returns 0 for an empty field as well
   * as for Stop, so "empty means use the default" could never have worked
   * -- offering the default as the initial value is both simpler and
   * better, since the user can see exactly what they are agreeing to. */
  strcpy(name, save_suggestion);
  if (!read_line(TEXT_STATUS_ROW, "Save as: ", name, SAVE_NAME_LEN))
    return; /* Stop, or the field was cleared */

  if (!choose_drive(TEXT_STATUS_ROW))
    return; /* cancelled, like clearing the filename field */

  strcpy(uiline, "Saving to drive ");
  append_uint(uiline, (unsigned int)(save_drive + SAVE_DEVICE_MIN));
  strcat(uiline, "...");
  gopher_putsxy(0, TEXT_STATUS_ROW, uiline);


  {
    char *sline = SCR_SLINE;
    unsigned int ln;
    unsigned char ci, err;

    err = cbmdos_create(name, save_drive);

    if (err == CBMDOS_OK) {
      for (ln = 0; ln < text_line_count && err == CBMDOS_OK; ln++) {
        text_get_line(ln, sline);
        for (ci = 0; sline[ci] && err == CBMDOS_OK; ci++)
          err = cbmdos_put(ascii_to_petscii((unsigned char)sline[ci]));
        if (err == CBMDOS_OK)
          err = cbmdos_put(0x0d); /* CBM text files end lines with CR */
      }
      if (err == CBMDOS_OK)
        err = cbmdos_close();
    }
    ok = (unsigned char)(err == CBMDOS_OK);
    save_err = err; /* surfaced in the failure message below */
  }

  /* Rebuild the 80-column mode the write tore down. It restores the
   * colours too, so the save/restore of $d020/$d021/$0286 that used to sit
   * here is gone -- it was not merely redundant but actively wrong, since
   * $0286 never tracked a colour chosen with F. */
  gopher_screen_init();

  /* Report by leaving the outcome in the pager's status line rather than
   * posting a message and waiting for a key.
   *
   * A modal "press any key" did not work: the write enables interrupts, so
   * the KERNAL keyboard scan runs again for its duration and leaves keys
   * queued -- including the Return that confirmed the filename -- and the
   * message was dismissed before it could be read. Draining $d610 and the
   * KERNAL buffer first did not reliably clear it either. Making the
   * result persist until the user's next keystroke sidesteps the race
   * entirely, and is better anyway: no extra keypress to acknowledge
   * something they asked for. */
  if (ok) {
    strcpy(save_result, "Saved to drive ");
    append_uint(save_result, (unsigned int)(save_drive + SAVE_DEVICE_MIN));
    strcat(save_result, ": ");
  }
  else {
    switch (save_err) {
    case CBMDOS_ERR_EXISTS:
      strcpy(save_result, "Already on disk - ");
      break;
    case CBMDOS_ERR_FULL:
      strcpy(save_result, "Disk full - ");
      break;
    case CBMDOS_ERR_DIRFULL:
      strcpy(save_result, "Directory full - ");
      break;
    case CBMDOS_ERR_PROT:
      strcpy(save_result, "Disk is write protected - ");
      break;
    default:
      strcpy(save_result, "SAVE FAILED - ");
      break;
    }
  }
  strcat(save_result, name);
  if (ok) {
    strcat(save_result, " (");
    append_uint(save_result, text_line_count);
    strcat(save_result, " lines)");
  }
}
#endif /* SAVE_ENABLED */

static void view_text(void)
{
  char *line = SCR_VIEWLINE;
  unsigned int top = 0;
  unsigned int maxtop;
  unsigned char row;
  unsigned char key;

  maxtop = (text_line_count > TEXT_ROWS) ? (text_line_count - TEXT_ROWS) : 0;

  while (1) {
    clrscr();
    for (row = 0; row < TEXT_ROWS; row++) {
      if (top + row >= text_line_count)
        break;
      text_get_line(top + row, line);
      gopher_putsxy(0, row, line);
    }

    if (text_line_count == 0)
      strcpy(uiline, "(empty file)  Stop:Back");
    else {
      strcpy(uiline, "Lines ");
      append_uint(uiline, top + 1);
      strcat(uiline, "-");
      append_uint(uiline, (top + TEXT_ROWS < text_line_count) ? (top + TEXT_ROWS) : text_line_count);
      strcat(uiline, " of ");
      append_uint(uiline, text_line_count);
      strcat(uiline, "  ");
      if (text_line_count >= text_max_lines)
        strcat(uiline, "(truncated)  ");
      strcat(uiline, "Up/Dn:Scroll Return:Page G:Get Stop:Back Help:Start");
    }
    if (save_result[0]) {
      /* Replace the whole status line: the outcome of an action the user
       * just asked for matters more than the key legend they already
       * know. Cleared by their next keystroke, below. */
      strcpy(uiline, save_result);
    }
    gopher_putsxy(0, TEXT_STATUS_ROW, uiline);

    flushkeybuf();
    key = cgetc();
    if (key >= 0xc1 && key <= 0xda)
      key = (unsigned char)(key & 0x7f);
    save_result[0] = 0; /* any keystroke clears the last save report */

    if (key == KEY_STOP)
      return;
    if (key == KEY_HELP_CODE) {
      want_chooser = 1;
      return;
    }
#if SAVE_ENABLED
    else if (key == KEY_GO_UPPER || key == KEY_GO_LOWER) {
      save_article();
      /* the loop redraws from the top, so nothing else to restore */
    }
#endif
    else if (key == KEY_CRSR_DOWN) {
      if (top < maxtop)
        top++;
    }
    else if (key == KEY_CRSR_UP) {
      if (top > 0)
        top--;
    }
    else if (key == KEY_RETURN) {
      /* page down, and wrap back to the top once the end is reached so a
       * short file is not a dead end */
      if (top >= maxtop)
        top = 0;
      else if (top + TEXT_ROWS > maxtop)
        top = maxtop;
      else
        top += TEXT_ROWS;
    }
    else if (key == KEY_HOME)
      top = 0;
  }
}

/* Item types 9 and 5: fetch a binary file to disk.
 *
 * The transfer is buffered in attic RAM and only written to disk once it
 * has completed, rather than streaming straight to the drive. Interleaving
 * F011 sector writes with the stack's pumping would stop the client draining
 * the receive ring for the duration of each write; the ring fills, the
 * peer's window closes, and recovering depends on a window update that
 * the earlier stack had its own history with. Buffering keeps the two
 * halves entirely separate, and the attic has room to spare.
 *
 * The cap is the capacity of a whole empty disk: cbmdos allocates across
 * tracks 1-39 and 41-80, skipping the directory track, so 79 tracks * 40
 * sectors * 254 bytes. Whatever the disk already holds comes off that, and
 * a transfer that does not fit is reported by the writer as "Disk full"
 * rather than being cut short here. */
#define DOWNLOAD_ATTIC_BASE 0x8100000UL
#define DOWNLOAD_MAX 802640UL /* 79 usable tracks * 40 sectors * 254 bytes */

static void download_binary(const char *host, const char *selector,
                            unsigned int port)
{
  static char name[SAVE_NAME_LEN + 1];
  unsigned char ip[4];
  unsigned char pump_result, b, i, got_any;
  unsigned char closing = 0, overflow = 0;
  unsigned int idle_frames;
  unsigned long total = 0;
  unsigned long shown = 0;
  unsigned char err;

  clrscr();
  gotoxy(0, 0);
  sayln("MEGA65 Gopher Client");
  newline();

  build_save_suggestion(selector);
  strcpy(name, save_suggestion);
  if (!read_line(4, "Download as: ", name, SAVE_NAME_LEN))
    return;
  if (!choose_drive(6))
    return;
  if (!choose_filetype(7))
    return;

  ui_set("Fetching ");
  ui_add(host);
  ui_add("...");
  gopher_putsxy(0, 8, uiline);

  if (!gopher_dns_resolve(host, ip)) {
    say_net_error(host, 0, 0);
    goto wait_and_return;
  }
  if (!gopher_tcp_connect(ip, port)) {
    gopher_dns_invalidate(host);
    say_net_error(host, ip, port);
    goto wait_and_return;
  }
  if (!gopher_tcp_send_selector(selector)) {
    say_net_error(host, ip, port);
    goto wait_and_return;
  }

  /* Straight to attic RAM, byte by byte. No line handling and no "."
   * terminator: a binary transfer ends when the peer closes. */
  idle_frames = 0;
  while (1) {
    if (tick()) {
      spin_step();
      pump_result = gopher_tcp_pump();
      got_any = 0;
      for (i = 0; i < 200; i++) {
        if (!gopher_tcp_recv_byte(&b))
          break;
        got_any = 1;
        if (total < DOWNLOAD_MAX)
          lpoke(DOWNLOAD_ATTIC_BASE + total, b);
        else
          overflow = 1;
        total++;
      }
      idle_frames = got_any ? 0 : (idle_frames + 1);
      if (pump_result)
        closing = 1;

      /* Progress every 4KB, so a long transfer visibly moves without
       * costing a screen write per packet. */
      if (total - shown >= 4096UL) {
        shown = total;
        ui_set("Received ");
        ui_add_uint((unsigned int)(total >> 10));
        ui_add("KB");
        gopher_putsxy(0, 9, uiline);
      }

      if ((closing && !got_any) ||
          idle_frames > (total ? IDLE_FRAMES_LIMIT : FIRST_BYTE_FRAMES_LIMIT))
        break;
    }
  }

  gopher_tcp_disconnect();

  if (total == 0) {
    gopher_putsxy(0, 9, "Nothing received.");
    goto wait_and_return;
  }
  if (overflow) {
    ui_set("Too large: ");
    ui_add_uint((unsigned int)(total >> 10));
    ui_add("KB exceeds the ");
    ui_add_uint((unsigned int)(DOWNLOAD_MAX >> 10));
    ui_add("KB a disk can take.");
    gopher_putsxy(0, 9, uiline);
    goto wait_and_return;
  }

  ui_set("Writing ");
  ui_add_uint((unsigned int)(total >> 10));
  ui_add("KB to drive ");
  ui_add_uint((unsigned int)(save_drive + SAVE_DEVICE_MIN));
  ui_add("...");
  gopher_putsxy(0, 9, uiline);

  err = cbmdos_create_as(name, save_drive,
                         save_as_prg ? CBMDOS_TYPE_PRG : CBMDOS_TYPE_SEQ);
  if (err == CBMDOS_OK) {
    unsigned long n;
    for (n = 0; n < total && err == CBMDOS_OK; n++)
      err = cbmdos_put(lpeek(DOWNLOAD_ATTIC_BASE + n));
    if (err == CBMDOS_OK)
      err = cbmdos_close();
  }

  gopher_screen_init(); /* the write tears the 80-column mode down */
  gotoxy(0, 0);
  sayln("MEGA65 Gopher Client");
  newline();

  if (err == CBMDOS_OK) {
    ui_set("Saved ");
    ui_add(name);
    ui_add(save_as_prg ? " (PRG) to drive " : " (SEQ) to drive ");
    ui_add_uint((unsigned int)(save_drive + SAVE_DEVICE_MIN));
    ui_add(" (");
    ui_add_uint(cbmdos_blocks());
    ui_add(" blocks)");
  }
  else {
    switch (err) {
    case CBMDOS_ERR_EXISTS:
      ui_set("Already on disk: ");
      break;
    case CBMDOS_ERR_FULL:
      ui_set("Disk full: ");
      break;
    case CBMDOS_ERR_DIRFULL:
      ui_set("Directory full: ");
      break;
    case CBMDOS_ERR_PROT:
      ui_set("Disk is write protected: ");
      break;
    default:
      ui_set("WRITE FAILED: ");
      break;
    }
    ui_add(name);
  }
  sayln(uiline);

wait_and_return:
  newline();
  sayln("[press any key to return to menu]");
  flushkeybuf();
  cgetc();
}

static void fetch_text_to(const char *host, const char *selector, unsigned int port, unsigned char view)
{
  unsigned char ip[4];
  char *linebuf = SCR_TEXT_LINE;
  unsigned char linelen = 0;
  unsigned char pump_result;
  unsigned char b, i;
  unsigned char got_any;
  unsigned char closing = 0;
  unsigned int idle_frames;
  char *foldbuf = SCR_FOLDBUF;
  char *p;

  clrscr();
  gotoxy(0, 0);
  text_clear();
  build_save_suggestion(selector);

  if (!gopher_dns_resolve(host, ip)) {
    say_net_error(host, 0, 0);
    goto wait_and_return;
  }
  if (!gopher_tcp_connect(ip, port)) {
    gopher_dns_invalidate(host);
    say_net_error(host, ip, port);
    goto wait_and_return;
  }
  if (!gopher_tcp_send_selector(selector)) {
    say_net_error(host, ip, port);
    goto wait_and_return;
  }

  sayln("Loading...");

  idle_frames = 0;
  while (1) {
    if (tick()) {
      spin_step();
      pump_result = gopher_tcp_pump();
      got_any = 0;
      for (i = 0; i < 200; i++) {
        if (!gopher_tcp_recv_byte(&b))
          break;
        got_any = 1;
        if (b == 0x0d)
          continue;
        if (b == 0x0a) {
          linebuf[linelen] = 0;
          if (linelen == 1 && linebuf[0] == 0x2e) /* lone "." terminator */
            goto show;
          gopher_fold_utf8(foldbuf, linebuf, TEXT_LINE_LEN);
          for (p = foldbuf; *p; p++)
            if (*p == 0x7b)
              *p = 0x28;
          text_add_line(foldbuf);
          linelen = 0;
          continue;
        }
        if (linelen < SCR_LINE_LEN - 1)
          linebuf[linelen++] = b;
      }
      idle_frames = got_any ? 0 : (idle_frames + 1);
      if (pump_result)
        closing = 1;
      /* The peer closing does NOT mean we have seen all its data: bytes
       * already sitting in the stack's receive ring still have to be
       * drained. Breaking straight out on pump_result threw them away --
       * a 60-line text file arrived as 31 lines. So once closure is
       * signalled, keep draining until a whole frame yields nothing. */
      if ((closing && !got_any) || idle_frames > IDLE_FRAMES_LIMIT)
        break;
    }
  }
  if (linelen) {
    linebuf[linelen] = 0;
    gopher_fold_utf8(foldbuf, linebuf, TEXT_LINE_LEN);
    text_add_line(foldbuf);
  }

show:
  if (view) {
    view_text();
    return;
  }
  /* G on a text item: to disk unseen, then the outcome, as the pager
   * shows it after its own G. */
  save_article();
  clrscr();
  gotoxy(0, 0);
  sayln(save_result);
  sayln("[press any key to return to menu]");
  flushkeybuf();
  cgetc();
  return;

wait_and_return:
  newline();
  sayln("[press any key to return to menu]");
  flushkeybuf();
  cgetc();
}

static const char *marker_for_type(unsigned char type)
{
  switch (type) {
  case GOPHER_TYPE_TEXT:
    return "[TXT] ";
  case GOPHER_TYPE_MENU:
    return "[DIR] ";
  case GOPHER_TYPE_SEARCH:
    return "[QRY] ";
  case GOPHER_TYPE_INFO:
    return "      ";
  case GOPHER_TYPE_HTML:
    return "[URL] ";
  case GOPHER_TYPE_BINARY:
  case GOPHER_TYPE_BINDOS:
    return "[BIN] ";
  case GOPHER_TYPE_GIF:
  case GOPHER_TYPE_IMAGE:
    return "[IMG] ";
  default:
    return "[?]   ";
  }
}

static unsigned char find_next_selectable(unsigned char from)
{
  unsigned char i;
  if (item_count == 0)
    return 0;
  for (i = 1; i <= item_count; i++) {
    unsigned char idx = (from + i) % item_count;
    if (item_is_selectable(item_get_type(idx)))
      return idx;
  }
  return from;
}

static unsigned char find_prev_selectable(unsigned char from)
{
  unsigned char i;
  if (item_count == 0)
    return 0;
  for (i = 1; i <= item_count; i++) {
    unsigned char idx = (from + item_count - i) % item_count;
    if (item_is_selectable(item_get_type(idx)))
      return idx;
  }
  return from;
}

static void draw_menu_page(struct location *loc, unsigned char selected, unsigned char page_start)
{
  unsigned char row, idx, type;
  static char display[ITEM_DISPLAY_LEN + 1];
  static char selector[ITEM_SELECTOR_LEN + 1];
  static char host[ITEM_HOST_LEN + 1];
  unsigned int port;

  clrscr();

  /* Title bar. The "gopher://" prefix is a C string literal, so CC65's
   * charmap already converted it to screen codes at compile time -- but
   * host/selector came off the network as raw ASCII and must be
   * translated at runtime (R-5). Translating the literal again would
   * corrupt it, so the two halves are built separately rather than with
   * one call. This is why the title rendered correctly on the home page
   * (literal host) but turned to garbage after following a link. */
  /* Built with the bounded helpers rather than strcpy/strcat.
   *
   * The arithmetic does currently fit -- "gopher://" + host(31) + ":" +
   * port(5) + "/" + selector(47) is 94 into uiline's 100 -- but it fits
   * only because three constants in gopher_items.h happen to sum below
   * SCR_UILINE_LEN, with nothing enforcing that. Raising ITEM_DISPLAY_LEN
   * or ITEM_SELECTOR_LEN would overflow silently. The helpers clamp. */
  ui_set("gopher://");
  ui_add(loc->host);
  /* Show a non-default port: without it, browsing host:7072 displayed as
   * plain "gopher://host/", which hides where you actually are. */
  if (loc->port != 70) {
    ui_add(":");
    ui_add_uint(loc->port);
  }
  ui_add("/");
  ui_add(loc->selector);
  if (ui_len > 79) {
    uiline[79] = 0;
    ui_len = 79;
  }
  gopher_putsxy(0, 0, uiline);

  for (row = 0; row < ITEMS_PER_PAGE; row++) {
    idx = page_start + row;
    if (idx >= item_count)
      break;
    item_get(idx, &type, display, selector, host, &port);
    /* ui_set() here also avoids repeating the construct that hung the
     * client: a string op applied to a function-returned pointer, which
     * is what `strcat(uiline, gopher_net_strerror())` was. That one
     * miscompiled into an infinite scan; this one did not, but the shape
     * is identical and it runs for every menu line. See 2.34. */
    ui_set(marker_for_type(type));
    ui_add(display);
    /* A marker plus a full-length display string can reach the right edge
     * of an 80-column line; anything past it wraps onto the next row and
     * overwrites the item drawn there. Clip instead. */
    if (ui_len > 79) {
      uiline[79] = 0;
      ui_len = 79;
    }
    if (idx == selected)
      revers(1);
    gopher_putsxy(0, ITEMS_TOP_ROW + row, uiline);
    if (idx == selected)
      revers(0);
  }

  /* Braces matter here: without them the else covered only the strcpy and
   * the three statements below ran unconditionally, so an empty menu
   * rendered as "(empty menu)1 of 0". */
  if (item_count == 0) {
    strcpy(uiline, "(empty menu)");
  }
  else {
    strcpy(uiline, "Item ");
    append_uint(uiline, selected + 1);
    strcat(uiline, " of ");
    append_uint(uiline, item_count);
    /* The text pager already says "(truncated)"; menus dropped everything
     * past MAX_ITEMS silently, which just looked like a short menu. */
    if (item_overflowed)
      strcat(uiline, "  (truncated)");
  }
  gopher_putsxy(0, ROW_STATUS, uiline);

  gopher_putsxy(0, ROW_HELP, "Up/Dn Return Stop:Back Home A:Address G:Get M:Mark F/B:Color Help:Start");
}

void main(void)
{
  unsigned char selected, page_start, key, type;
  static char display[ITEM_DISPLAY_LEN + 1];
  static char selector[ITEM_SELECTOR_LEN + 1];
  static char host[ITEM_HOST_LEN + 1];
  unsigned int port;
  unsigned char have_menu;
  unsigned char restored;
  const char *boot_err;



  /* The saved-start-page config is not read here any more: bookmarks are
   * disabled (see REQUIREMENTS.md) and the startup chooser below decides
   * where to begin, so there is nothing for it to override. */

  gopher_screen_init();

  sayln("MEGA65 Gopher Client - version " GOPHER_VERSION);
  newline();

  /* mega-net's image and the trampoline are separate binaries that must be in
   * place before any network call. Dev builds inject them with
   * `m65 -@`; a standalone PRG loads them from the SD card itself. */
  sayln("Loading network stack...");
  if (!gopher_boot_load(&boot_err)) {
    unsigned char di, dv;
    sayln(boot_err);
    /* ETHBIN ships inside the .d81 alongside the client, so a failure here
     * is nearly always the wrong disk being mounted. List what each drive
     * holds -- that distinguishes a bad mount from a bad loader without
     * needing a host-side probe. */
    for (dv = 0; dv < 2; dv++) {
      strcpy(uiline, "Drive ");
      append_uint(uiline, (unsigned int)(dv + 8));
      strcat(uiline, " contains:");
      sayln(uiline);
      if (cbmdos_dir_open(dv) != 0) {
        sayln("  <no disk, or directory unreadable>");
        continue;
      }
      for (di = 0; di < 8; di++) {
        if (cbmdos_dir_get(di, SCR_DOS_LINE)) {
          gopher_puts("  ");
          sayln(SCR_DOS_LINE);
        }
      }
    }
    while (1)
      ;
  }

  /* Default saves to the disk the user actually started from: booting off
   * drive 9 and then being offered drive 8 is a needless second surprise.
   * Still fully overridable at the prompt. */
  save_drive = boot_drive;

  text_init(); /* pick the text store before anything can use it */
  item_snapshot_init(); /* menu cache for Back; probes its own attic region */
  sayln("Bringing up network...");
  if (!gopher_net_init()) {
    sayln("ERROR: could not obtain a DHCP lease");

    while (1)
      ;
  }

  /* A start page saved with M on a previous run, if the boot disk has one.
   * Offered as an extra option rather than applied silently: the disk is
   * also the distribution medium, so an image shared after bookmarking
   * would otherwise redirect its recipient without explanation. */
  bookmark_count = config_load(bookmarks, CFG_MAX_BOOKMARKS, boot_drive);
  if (bookmark_count == CFG_NO_FILE) {
    /* Never configured: seed the two sites that used to be hard-coded
     * options. They are ordinary bookmarks, so they can be deleted.
     *
     * Deliberately NOT written to disk here. Writing on every first run
     * would touch a disk the user may not want written to, and it is
     * unnecessary: the file gets written the moment they add or remove
     * anything, and until then re-seeding on each run gives the same
     * result. An empty file, once written, is what stops them coming
     * back -- see CFG_NO_FILE. */
    /* gopherpedia: straight to the English edition rather than the root,
     * which opens on a language picker whose first selectable entry is
     * German. Selector confirmed against the live server: English is
     * "1English<TAB>/lang=en<TAB>gopherpedia.com<TAB>70". */
    set_location(&bookmarks[0], "gopherpedia.com", "/lang=en", 70);
    set_location(&bookmarks[1], "gopher.floodgap.com", "", 70);
    bookmark_count = 2;
  }

  /* Ask where to start. Whatever is chosen also becomes the Home target,
   * so the Home key returns there rather than to a compiled-in default. */
  choose_start_site(&home_loc);
  current_loc = home_loc;

  restored = 0;
  while (1) {
    if (restored) {
      /* Came back to a page we already had: selected and page_start were
       * restored with it, so there is nothing to fetch or reset. */
      restored = 0;
      have_menu = 1;
      goto have_page;
    }

    if (want_chooser) {
      want_chooser = 0;
      choose_start_site(&home_loc);
      current_loc = home_loc;
      history_count = 0;
    }

    have_menu = fetch_menu(&current_loc);
    if (!have_menu) {
      newline();
      sayln("[press any key]");
      flushkeybuf();
      cgetc();
      if (history_count > 0) {
        restored = history_pop(&current_loc, &selected, &page_start);
        continue;
      }

      /* Nothing to go back to. Falling back to home_loc here used to trap
       * the user: if the start site itself was unreachable -- a typo at
       * the chooser, or a host that is simply down -- home_loc WAS the
       * failing location, so this refetched it, failed again, and looped
       * forever. The menu never drew, so G:URL and Home were unreachable
       * and only a reset got out.
       *
       * Return to the chooser instead. It costs one keypress to retry the
       * same site, and it is the only screen that can reach URL entry
       * without a menu behind it. */
      choose_start_site(&home_loc);
      current_loc = home_loc;
      history_count = 0;
      continue;
    }

    selected = find_next_selectable(item_count - 1);
    page_start = 0;

have_page:
    while (1) {
      if (selected < page_start)
        page_start = selected;
      if (selected >= page_start + ITEMS_PER_PAGE)
        page_start = selected - ITEMS_PER_PAGE + 1;

      draw_menu_page(&current_loc, selected, page_start);

      flushkeybuf();
      key = cgetc();
      if (key >= 0xc1 && key <= 0xda) /* MEGA+letter: the capital with bit 7 set (ssh 5.29) */
        key = (unsigned char)(key & 0x7f);

      if (key == KEY_CRSR_DOWN) {
        selected = find_next_selectable(selected);
      }
      else if (key == KEY_CRSR_UP) {
        selected = find_prev_selectable(selected);
      }
      else if (key == KEY_RETURN) {
        if (item_count == 0)
          continue;
        item_get(selected, &type, display, selector, host, &port);
        if (type == GOPHER_TYPE_MENU) {
          history_push(&current_loc, selected, page_start);
          set_location(&current_loc, host, selector, port);
          break; /* refetch */
        }
        else if (type == GOPHER_TYPE_TEXT) {
          fetch_text(host, selector, port);
          if (want_chooser) /* HELP in the pager */
            break;
          /* items are still in the store -- just redraw, no refetch */
        }
        else if (type == GOPHER_TYPE_BINARY || type == GOPHER_TYPE_BINDOS ||
                 type == GOPHER_TYPE_GIF || type == GOPHER_TYPE_IMAGE) {
          download_binary(host, selector, port);
          /* same: the menu is untouched, so just redraw */
        }
        else if (type == GOPHER_TYPE_SEARCH) {
          /* RFC 1436 search: the request is the item's selector, a TAB,
           * then the query. The reply is an ordinary menu, so once the
           * combined string is built this is just a normal fetch.
           *
           * Storing the combined string in the location is what makes
           * Back work: returning to a search re-runs it. */
          /* The query is read directly into the tail of `combined`,
           * after the selector and TAB, so no separate query buffer is
           * needed -- it only ever existed to be concatenated here. */
          char *combined = SCR_COMBINED;
          unsigned char sellen = (unsigned char)strlen(selector);
          unsigned char maxq;

          /* leave room for the selector and the TAB */
          maxq = (sellen + 1 < LOC_SELECTOR_LEN) ? (unsigned char)(LOC_SELECTOR_LEN - sellen - 1) : 0;

          if (maxq) {
            strcpy(combined, selector);
            combined[sellen] = 0x09; /* TAB */
            combined[sellen + 1] = 0;
            combined[sellen + 1] = 0; /* empty initial value */
            if (read_line(ROW_STATUS, "Search for: ", combined + sellen + 1, maxq)) {
              history_push(&current_loc, selected, page_start);
              set_location(&current_loc, host, combined, port);
              break; /* refetch: the results are a menu */
            }
          }
        }
      }
      else if (key == KEY_STOP) {
        if (history_count > 0) {
          /* Redraw from the snapshot when we have one; only fall back to
           * the network if attic RAM was unavailable. */
          restored = history_pop(&current_loc, &selected, &page_start);
          break;
        }
        want_chooser = 1; /* nothing behind this menu: the start screen */
        break;
      }
      else if (key == KEY_HELP_CODE) {
        want_chooser = 1;
        break;
      }
      else if (key == KEY_GO_UPPER || key == KEY_GO_LOWER) {
        /* G: the selected item to disk, unseen; RETURN views it instead */
        if (item_count == 0)
          continue;
        item_get(selected, &type, display, selector, host, &port);
        if (type == GOPHER_TYPE_TEXT) {
          fetch_text_to(host, selector, port, 0);
          if (want_chooser)
            break;
        }
        else if (type == GOPHER_TYPE_BINARY || type == GOPHER_TYPE_BINDOS ||
                 type == GOPHER_TYPE_GIF || type == GOPHER_TYPE_IMAGE) {
          download_binary(host, selector, port);
        }
        else {
          gopher_putsxy(0, ROW_STATUS, "Only a file can be got: a text, a binary or an image. RETURN opens a menu.");
          flushkeybuf();
          cgetc();
        }
      }
      else if (key == KEY_ADDR_UPPER || key == KEY_ADDR_LOWER) {
        char *urlbuf = SCR_URLBUF;
        urlbuf[0] = 0; /* read_line now treats `out` as an initial value */
        if (read_line(ROW_STATUS, "Go to: ", urlbuf, SCR_URLBUF_LEN - 1)) {
          struct location dest;
          unsigned char utype = GOPHER_TYPE_MENU;
          if (parse_url(urlbuf, &dest, &utype)) {
            if (utype == GOPHER_TYPE_TEXT) {
              /* Read in place, exactly as activating a [TXT] item does:
               * the current menu is still in the item store, so there is
               * nothing to refetch and no history level to push. The
               * loop redraws at the top of the next iteration. */
              fetch_text(dest.host, dest.selector, dest.port);
              if (want_chooser)
                break;
              continue;
            }
            history_push(&current_loc, selected, page_start);
            current_loc = dest;
            break; /* refetch */
          }
        }
        /* cancelled or unparseable: fall through and redraw */
      }
      else if (key == KEY_MARK_UPPER || key == KEY_MARK_LOWER) {
        /* Toggle: bookmark the page being viewed, or remove it if it is
         * already bookmarked. One key does both, which avoids needing a
         * separate management screen to delete entries.
         *
         * The list goes on the disk the client booted from, so it travels
         * with the program. The write tears down the 80-column mode,
         * hence the rebuild afterwards. */
        unsigned char at = bookmark_index(&current_loc);
        unsigned char removed = 0, full = 0, ok = 0;

        if (at < CFG_MAX_BOOKMARKS) {
          unsigned char j;
          for (j = at; j + 1 < bookmark_count; j++)
            bookmarks[j] = bookmarks[j + 1];
          bookmark_count--;
          removed = 1;
        }
        else if (bookmark_count >= CFG_MAX_BOOKMARKS) {
          full = 1;
        }
        else {
          bookmarks[bookmark_count++] = current_loc;
        }

        if (!full) {
          ok = config_save(bookmarks, bookmark_count, boot_drive);
          gopher_screen_init();
        }

        if (full) {
          ui_set("Bookmark list is full (");
          ui_add_uint(CFG_MAX_BOOKMARKS);
          ui_add(").");
        }
        else if (!ok) {
          ui_set("Could not write bookmarks to drive ");
          ui_add_uint((unsigned int)(boot_drive + SAVE_DEVICE_MIN));
        }
        else {
          ui_set(removed ? "Bookmark removed: " : "Bookmarked: ");
          ui_add(current_loc.host);
        }
        gopher_putsxy(0, ROW_STATUS, uiline);
        flushkeybuf();
        cgetc();
      }
      else if (key == KEY_FG_UPPER || key == KEY_FG_LOWER) {
        gopher_screen_cycle_text_colour();
      }
      else if (key == KEY_BG_UPPER || key == KEY_BG_LOWER) {
        /* Background and border move together and stay identical, so the
         * screen reads as a single surface. */
        gopher_screen_cycle_background();
      }
      else if (key == KEY_HOME) {
        /* Home goes to the start page. Pressing it again when already
         * there returns to the start screen, so the bookmark list is
         * reachable without restarting the program -- there is nowhere
         * "above" home to go otherwise, and the key would do nothing. */
        if (current_loc.port == home_loc.port &&
            !strcmp(current_loc.host, home_loc.host) &&
            !strcmp(current_loc.selector, home_loc.selector)) {
          want_chooser = 1;
          break;
        }
        current_loc = home_loc;
        history_count = 0;
        break; /* refetch */
      }
    }
  }
}
