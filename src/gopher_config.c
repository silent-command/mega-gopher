#include <string.h>
#include "gopher_loc.h"
#include "gopher_config.h"
#include "gopher_cbmdos.h"


/* The old KERNAL-era names carried CBM DOS open modes ("@:NAME,S,W").
 * The F011 layer takes a bare filename and handles replacement through
 * cbmdos_delete, so those are gone. */
#define CFG_MAGIC "GOPHER2"     /* a list of bookmarks */
#define CFG_MAGIC_V1 "GOPHER1"  /* one saved start page; still readable */

/* Big enough for CFG_MAX_BOOKMARKS entries at their worst case: host
 * (31) + port (5) + selector (80), each with a newline, plus the magic
 * line. Undersizing this would silently truncate the last bookmarks on
 * save rather than failing. */
static char buf[2048];

/* Built with an explicit running length, never strcat/strlen.
 *
 * strcat(buf, ...) here compiled to a scan loop whose pointer bytes were
 * reloaded each iteration from zero-page locations the loop never
 * updated, so it re-read one byte forever and hung the machine at the
 * start screen. That is the SECOND time this exact llvm-mos pattern has
 * appeared -- say_net_error had it too (REQUIREMENTS.md 2.34) -- and it
 * surfaced here only when buf grew from 1024 to 2048 bytes, which was
 * enough to change code generation.
 *
 * Keeping the length rather than re-deriving it avoids the construct
 * entirely, and is faster besides: appending N strings no longer rescans
 * the buffer N times. */
/* The write position is passed in and returned rather than held in a
 * file-scope variable.
 *
 * It WAS a static, and the compiler did not keep it in step: the appended
 * text landed correctly but the static never advanced past the first
 * append, so the closing `buf[len] = 0` wrote its NUL back over the first
 * byte of the second field, and the caller then wrote only 8 bytes.
 * Observed directly -- buf held "GOPHER2\n" then a NUL where 'g' should
 * have been, followed by "opher.floodgap.com". Locals threaded through
 * the call avoid depending on that. */
static unsigned int append(unsigned int at, const char *src)
{
  while (*src && at < sizeof(buf) - 2)
    buf[at++] = *src++;
  buf[at++] = 0x0a;
  buf[at] = 0;
  return at;
}

static unsigned int append_uint(unsigned int at, unsigned int v)
{
  char d[6];
  unsigned char n = 0;

  if (!v)
    d[n++] = 0x30;
  while (v) {
    d[n++] = (char)(0x30 + (v % 10));
    v /= 10;
  }
  while (n && at < sizeof(buf) - 2)
    buf[at++] = d[--n];
  buf[at++] = 0x0a;
  buf[at] = 0;
  return at;
}

/* Disk access, on the F011/CBM DOS stack.
 *
 * This was dead code for most of the project. The original went through
 * the KERNAL, which was made to work in isolation but destabilised the
 * client: $d030 carries VIC-III/IV configuration as well as ROM banking,
 * so switching it fights the 80-column screen the client sets up. The old
 * comment here predicted the fix -- "a safe implementation probably has to
 * avoid the KERNAL altogether, driving the F011 floppy controller
 * directly" -- and that is exactly what gopher_cbmdos.c now provides, so
 * the feature is simply rewired onto it. Neither $d030 nor the interrupt
 * state is disturbed.
 *
 * The file is written to whichever drive the client booted from, so the
 * bookmark travels with the program disk. Note that means a bookmark saved
 * before sharing the image becomes the recipient's start page too. */
#define CFG_NAME "GOPHER.CFG"

unsigned char config_save(const struct location *list, unsigned char count,
                          unsigned char drive)
{
  unsigned char err, i;
  unsigned int j, len;

  if (count > CFG_MAX_BOOKMARKS)
    count = CFG_MAX_BOOKMARKS;

  buf[0] = 0;
  len = append(0, CFG_MAGIC);
  for (i = 0; i < count; i++) {
    len = append(len, list[i].host);
    len = append_uint(len, list[i].port);
    len = append(len, list[i].selector);
  }

  /* cbmdos_create refuses duplicates, so rewriting has to remove the
   * previous file first. A missing file is not an error here. */
  err = cbmdos_delete(CFG_NAME, drive);
  if (err != CBMDOS_OK && err != CBMDOS_ERR_NOTFOUND)
    return 0;

  /* An empty list still writes a file, holding only the magic line. An
   * absent file and an empty one mean different things at startup: absent
   * seeds the built-in start sites, empty means the user removed them.
   * Deleting the file here would resurrect them on the next run. */

  if (cbmdos_create(CFG_NAME, drive) != CBMDOS_OK)
    return 0;
  for (j = 0; j < len; j++)
    if (cbmdos_put((unsigned char)buf[j]) != CBMDOS_OK)
      return 0;
  return (unsigned char)(cbmdos_close() == CBMDOS_OK);
}

/* Returns a pointer just past the next newline, or 0 at end of data. */
static char *next_line(char *p)
{
  while (*p && *p != 0x0a)
    p++;
  if (!*p)
    return 0;
  *p = 0;
  return p + 1;
}

unsigned char config_load(struct location *list, unsigned char max,
                          unsigned char drive)
{
  char *cur, *host, *port, *sel, *next, *q;
  unsigned char n = 0;
  unsigned long got;

  /* buf lives below $d000, so its 16-bit address is a valid 28-bit
   * destination for the loader. */
  got = cbmdos_load(CFG_NAME, drive, (unsigned long)(unsigned int)buf,
                    (unsigned long)(sizeof(buf) - 1));
  if (got == 0)
    return CFG_NO_FILE; /* no config at all -- caller seeds the defaults */
  buf[got] = 0;

  cur = next_line(buf); /* terminates the magic line; cur = first host */
  if (!cur)
    return 0;
  /* GOPHER1 held exactly one entry, GOPHER2 holds a list, and both parse
   * identically from here -- so a config written by an older build still
   * works rather than being silently discarded. */
  if (strcmp(buf, CFG_MAGIC) && strcmp(buf, CFG_MAGIC_V1))
    return 0; /* not ours, or newer -- ignore rather than guess */

  while (n < max && cur && *cur) {
    unsigned int v = 0;

    host = cur;
    port = next_line(host);
    if (!port)
      break;
    sel = next_line(port);
    if (!sel)
      break;
    next = next_line(sel); /* 0 at end of data, else the following host */

    for (q = port; *q >= 0x30 && *q <= 0x39; q++)
      v = v * 10 + (unsigned int)(*q - 0x30);
    if (v == 0)
      v = 70;

    strncpy(list[n].host, host, LOC_HOST_LEN);
    list[n].host[LOC_HOST_LEN] = 0;
    strncpy(list[n].selector, sel, LOC_SELECTOR_LEN);
    list[n].selector[LOC_SELECTOR_LEN] = 0;
    list[n].port = v;
    n++;

    cur = next;
  }
  return n;
}
