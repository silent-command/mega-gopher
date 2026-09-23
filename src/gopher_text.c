#include <string.h>
#include "mega65/memory.h"
#include "gopher_text.h"

unsigned int text_line_count = 0;
unsigned int text_max_lines = MAX_TEXT_LINES_CHIP;

static unsigned long text_base = TEXT_BASE_CHIP;
static unsigned char using_attic = 0;

/* Attic RAM is optional hardware, so prove it is there and that it holds
 * what we write before trusting it. The same readback technique caught
 * bank 6 not existing at all, and banks 2/3 being ROM rather than RAM. */
static unsigned char attic_present(void)
{
  unsigned long a;
  unsigned char i;

  for (i = 0; i < 4; i++) {
    a = TEXT_BASE_ATTIC + ((unsigned long)i << 12);
    lpoke(a, (unsigned char)(0x5a + i));
  }
  for (i = 0; i < 4; i++) {
    a = TEXT_BASE_ATTIC + ((unsigned long)i << 12);
    if (lpeek(a) != (unsigned char)(0x5a + i))
      return 0;
  }
  return 1;
}

void text_init(void)
{
  if (attic_present()) {
    text_base = TEXT_BASE_ATTIC;
    text_max_lines = MAX_TEXT_LINES_ATTIC;
    using_attic = 1;
  }
  else {
    text_base = TEXT_BASE_CHIP;
    text_max_lines = MAX_TEXT_LINES_CHIP;
    using_attic = 0;
  }
}

unsigned char text_using_attic(void)
{
  return using_attic;
}

void text_clear(void)
{
  text_line_count = 0;
}

unsigned char text_add_line(const char *line)
{
  static char rec[TEXT_REC_SIZE];
  unsigned char len;

  if (text_line_count >= text_max_lines)
    return 0;

  len = (unsigned char)strlen(line);
  if (len > TEXT_LINE_LEN)
    len = TEXT_LINE_LEN;
  memcpy(rec, line, len);

  /* Zero-pad and always copy a whole fixed-size record. Keeping the count
   * constant also keeps it away from 0, which DMAgic would treat as
   * 65536 -- see the note in gopher_screen.c. */
  memset(rec + len, 0, TEXT_REC_SIZE - len);

  lcopy((unsigned long)rec, text_base + (unsigned long)text_line_count * TEXT_REC_SIZE, TEXT_REC_SIZE);
  text_line_count++;
  return 1;
}

void text_get_line(unsigned int index, char *out)
{
  if (index >= text_line_count) {
    out[0] = 0;
    return;
  }
  lcopy(text_base + (unsigned long)index * TEXT_REC_SIZE, (unsigned long)out, TEXT_REC_SIZE);
  out[TEXT_LINE_LEN] = 0;
}
