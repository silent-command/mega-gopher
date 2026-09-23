/* Line store for a fetched text file (FR-9/FR-10/FR-11 paging).
 *
 * Capacity comes from attic (expansion) RAM at $8000000 when the machine
 * has it -- verified by readback at startup, not assumed, because it is
 * optional hardware. That allows MAX_TEXT_LINES_ATTIC lines, which is
 * more than any gopher text file is likely to hold.
 *
 * Without attic RAM we fall back to the tail of bank 5, which only has
 * room for MAX_TEXT_LINES_CHIP lines. Bank 5 is tight: the RX ring, the
 * item store and this share it (see gopher_items.h). Banks 2 and 3 are
 * NOT available -- $20000-$3ffff is the 128KB C65 ROM, confirmed by a
 * readback probe where 126 of 128 samples failed to stick.
 *
 * Lines are fixed-width records so "show line N" is a single multiply,
 * which is what a pager does constantly. */
#ifndef GOPHER_TEXT_H
#define GOPHER_TEXT_H

#define TEXT_LINE_LEN 79 /* 80-column screen, minus room for the NUL */
#define TEXT_REC_SIZE 80

#define TEXT_BASE_ATTIC 0x8000000UL
#define TEXT_BASE_CHIP 0x5a000UL

#define MAX_TEXT_LINES_ATTIC 4000u /* 320KB of attic RAM */
#define MAX_TEXT_LINES_CHIP 250u   /* what fits in bank 5's tail */

extern unsigned int text_line_count;
extern unsigned int text_max_lines;

/* Probes for attic RAM and selects the store. Call once at startup. */
void text_init(void);

/* 1 if the large attic-RAM store is in use, 0 if the small fallback. */
unsigned char text_using_attic(void);

void text_clear(void);

/* Stores one line, truncated to TEXT_LINE_LEN. Returns 0 once full, so
 * callers can stop buffering (the connection should still be drained). */
unsigned char text_add_line(const char *line);

/* Reads line `index` into out, which must hold TEXT_LINE_LEN+1 bytes. */
void text_get_line(unsigned int index, char *out);

#endif
