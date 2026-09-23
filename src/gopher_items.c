#include <string.h>
#include "mega65/memory.h"
#include "gopher_items.h"
#include "gopher_scratch.h"

unsigned char item_count = 0;
unsigned char item_overflowed = 0;

/* Field offsets within one ITEM_SIZE-byte record. */
#define OFS_TYPE 0
#define OFS_DISPLAY 1
#define OFS_SELECTOR (OFS_DISPLAY + ITEM_DISPLAY_LEN + 1)
#define OFS_HOST (OFS_SELECTOR + ITEM_SELECTOR_LEN + 1)
#define OFS_PORT (OFS_HOST + ITEM_HOST_LEN + 1)

/* Scratch buffer for building/reading one record -- avoids putting a
 * 160-byte buffer on the stack in every caller. Not reentrant, but
 * nothing here needs to be. */
/* In fixed low RAM rather than .bss; see gopher_scratch.h. */
#define rec SCR_ITEM_REC

static void put_field(unsigned char offset, const char *src, unsigned char maxlen)
{
  unsigned char i;
  for (i = 0; i < maxlen && src[i]; i++)
    rec[offset + i] = src[i];
  rec[offset + i] = 0;
}

static void get_field(unsigned char offset, char *dst, unsigned char maxlen)
{
  unsigned char i;
  for (i = 0; i < maxlen && rec[offset + i]; i++)
    dst[i] = rec[offset + i];
  dst[i] = 0;
}

void item_clear_all(void)
{
  item_count = 0;
  item_overflowed = 0;
}

unsigned char item_add(unsigned char type, const char *display, const char *selector, const char *host,
    unsigned int port)
{
  if (item_count >= MAX_ITEMS) {
    item_overflowed = 1;
    return 0;
  }

  rec[OFS_TYPE] = type;
  put_field(OFS_DISPLAY, display, ITEM_DISPLAY_LEN);
  put_field(OFS_SELECTOR, selector, ITEM_SELECTOR_LEN);
  put_field(OFS_HOST, host, ITEM_HOST_LEN);
  rec[OFS_PORT] = port & 0xff;
  rec[OFS_PORT + 1] = port >> 8;

  lcopy((unsigned long)rec, ITEMS_BASE + (unsigned long)item_count * ITEM_SIZE, ITEM_SIZE);
  item_count++;
  return 1;
}

void item_get(unsigned char index, unsigned char *type, char *display, char *selector, char *host,
    unsigned int *port)
{
  lcopy(ITEMS_BASE + (unsigned long)index * ITEM_SIZE, (unsigned long)rec, ITEM_SIZE);

  *type = rec[OFS_TYPE];
  get_field(OFS_DISPLAY, display, ITEM_DISPLAY_LEN);
  get_field(OFS_SELECTOR, selector, ITEM_SELECTOR_LEN);
  get_field(OFS_HOST, host, ITEM_HOST_LEN);
  *port = rec[OFS_PORT] | (rec[OFS_PORT + 1] << 8);
}

unsigned char item_get_type(unsigned char index)
{
  /* lpeek(), not a full-record lcopy() -- only need one byte. */
  return lpeek(ITEMS_BASE + (unsigned long)index * ITEM_SIZE + OFS_TYPE);
}

/* --- menu snapshots --------------------------------------------------
 *
 * Placed clear of the text store, which occupies
 * TEXT_BASE_ATTIC ($8000000) + 4000 * 80 = $804e200. */
#define MENU_CACHE_BASE 0x8060000UL
#define MENU_SLOT_SIZE ((unsigned long)MAX_ITEMS * ITEM_SIZE) /* 32000 */

static unsigned char snap_ok = 0;
static unsigned char snap_count[ITEM_SNAPSHOT_SLOTS];
static unsigned char snap_valid[ITEM_SNAPSHOT_SLOTS];

void item_snapshot_init(void)
{
  unsigned char i;
  unsigned long a;

  /* Probe the first byte of the first and last slot rather than trusting
   * the text store's probe: this region is 384KB further up, and a
   * readback is the only thing that has ever been reliable here. */
  snap_ok = 1;
  for (i = 0; i < 2; i++) {
    a = MENU_CACHE_BASE + (unsigned long)(i ? (ITEM_SNAPSHOT_SLOTS - 1) : 0) * MENU_SLOT_SIZE;
    lpoke(a, (unsigned char)(0xa7 + i));
    if (lpeek(a) != (unsigned char)(0xa7 + i))
      snap_ok = 0;
  }

  for (i = 0; i < ITEM_SNAPSHOT_SLOTS; i++)
    snap_valid[i] = 0;
}

unsigned char item_snapshot_save(unsigned char slot)
{
  if (!snap_ok || slot >= ITEM_SNAPSHOT_SLOTS)
    return 0;

  snap_count[slot] = item_count;
  snap_valid[slot] = 1;

  /* A zero-length DMA copies 65536 bytes on this hardware, so an empty
   * menu must skip the copy entirely rather than request 0 bytes. */
  if (item_count)
    lcopy(ITEMS_BASE, MENU_CACHE_BASE + (unsigned long)slot * MENU_SLOT_SIZE,
        (unsigned int)item_count * ITEM_SIZE);
  return 1;
}

unsigned char item_snapshot_load(unsigned char slot)
{
  if (!snap_ok || slot >= ITEM_SNAPSHOT_SLOTS || !snap_valid[slot])
    return 0;

  item_count = snap_count[slot];
  item_overflowed = 0;
  if (item_count)
    lcopy(MENU_CACHE_BASE + (unsigned long)slot * MENU_SLOT_SIZE, ITEMS_BASE,
        (unsigned int)item_count * ITEM_SIZE);
  return 1;
}

unsigned char item_is_selectable(unsigned char type)
{
  return type == GOPHER_TYPE_TEXT || type == GOPHER_TYPE_MENU ||
         type == GOPHER_TYPE_SEARCH || type == GOPHER_TYPE_BINARY ||
         type == GOPHER_TYPE_BINDOS || type == GOPHER_TYPE_GIF ||
         type == GOPHER_TYPE_IMAGE;
}
