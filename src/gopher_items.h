/* Storage for a fetched gopher menu's items (type/display/selector/host/
 * port per item), needed for phase 5 navigation: the user needs to move
 * a selection cursor and follow a link *after* the whole menu has
 * loaded, so (unlike phase 4's stream-and-print) we now have to hold
 * the parsed items somewhere.
 *
 * Held in bank 1 (physical $12000 up, 32000 bytes), not normal C64
 * address space -- our compiled program plus mega65-libc already eats
 * into the ~38KB a C64-family program has to work with, and a menu can
 * have many items. Bank 1 is the program's own: mega-net leaves it
 * alone (its README's memory table), and the only other thing there is
 * the font copy at $11000-$117FF. The store sat at $52000 in bank 5
 * until 2026-09-06, inside mega-net's socket buffers ($50000-$5E8FF),
 * which held only because the client opens one socket (§2.36). Bank 4
 * is mega-net's code; banks 6 and up are not memory here (R-7). */
#ifndef GOPHER_ITEMS_H
#define GOPHER_ITEMS_H

#define ITEM_DISPLAY_LEN 71
#define ITEM_SELECTOR_LEN 47
#define ITEM_HOST_LEN 31
#define ITEM_SIZE 160 /* 1 + 72 + 48 + 32 + 2 = 155, rounded up */
#define ITEMS_BASE 0x12000L
#define MAX_ITEMS 200

extern unsigned char item_count;

/* Set when a menu had more than MAX_ITEMS entries and the excess was
 * dropped, so the UI can say so instead of silently showing a short menu.
 * Cleared by item_clear_all(). */
extern unsigned char item_overflowed;

/* Resets the store to empty. Call before fetching a new menu. */
void item_clear_all(void);

/* Copies (truncating as needed) into a new item record. Returns 1 if
 * added, 0 if the store is full. */
unsigned char item_add(unsigned char type, const char *display, const char *selector, const char *host,
    unsigned int port);

/* Reads item `index` back into caller-supplied buffers. display must be
 * at least ITEM_DISPLAY_LEN+1 bytes, selector ITEM_SELECTOR_LEN+1, host
 * ITEM_HOST_LEN+1. */
void item_get(unsigned char index, unsigned char *type, char *display, char *selector, char *host,
    unsigned int *port);

/* Lightweight: reads just the type byte, for code (like scanning for the
 * next/previous selectable item) that doesn't need the string fields.
 * Using the full item_get() for this was a real bug (fixed): passing
 * 1-byte dummy buffers for display/selector/host while item_get() writes
 * up to 72/48/32 bytes into them silently smashed the stack for any
 * real (i.e. longer than 1 byte) menu text. */
unsigned char item_get_type(unsigned char index);

/* --- menu snapshots (Back without a refetch) --------------------------
 *
 * The item store holds exactly one menu, so following a link overwrites
 * the parent and Back had to re-fetch it from the network. These copy the
 * whole store to and from attic RAM, one slot per history level, which
 * makes Back instant and costs the server nothing.
 *
 * Attic RAM is optional hardware, so item_snapshot_init() probes it the
 * same way gopher_text.c does; if it is absent every call below returns 0
 * and the caller falls back to re-fetching. */
#define ITEM_SNAPSHOT_SLOTS 8

/* Probes the cache region. Call once at startup, after text_init(). */
void item_snapshot_init(void);

/* Copies the current store into `slot`. Returns 1 if it was stored. */
unsigned char item_snapshot_save(unsigned char slot);

/* Restores `slot` into the store, setting item_count. Returns 1 on
 * success, 0 if that slot holds nothing usable. */
unsigned char item_snapshot_load(unsigned char slot);

/* Item type numeric constants (see R-5 in REQUIREMENTS.md: never compare
 * against char literals like '0'/'1' for bytes that might be raw
 * network ASCII -- CC65's compile-time charmap translates char literals,
 * so such comparisons silently fail to match). */
#define GOPHER_TYPE_TEXT 0x30   /* '0' */
#define GOPHER_TYPE_MENU 0x31   /* '1' */
#define GOPHER_TYPE_SEARCH 0x37 /* '7' */
#define GOPHER_TYPE_INFO 0x69   /* 'i' */
#define GOPHER_TYPE_HTML 0x68   /* 'h' */
#define GOPHER_TYPE_BINARY 0x39 /* '9' -- generic binary file */
#define GOPHER_TYPE_BINDOS 0x35 /* '5' -- "DOS binary", same handling */
/* Images cannot be displayed on this machine, but they are just binary
 * files, so they are offered as downloads rather than shown as
 * unsupported. */
#define GOPHER_TYPE_GIF 0x67    /* 'g' */
#define GOPHER_TYPE_IMAGE 0x49  /* 'I' */

/* Selectable = the user can navigate to it (FR-6). */
unsigned char item_is_selectable(unsigned char type);

#endif
